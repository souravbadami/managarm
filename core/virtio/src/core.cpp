
#include <assert.h>
#include <iostream>
#include <optional>

#include <core/virtio/core.hpp>
#include <fafnir/dsl.hpp>
#include <protocols/kernlet/compiler.hpp>

namespace virtio_core {

struct Mapping {
	static constexpr size_t pageSize = 0x1000;

	friend void swap(Mapping &x, Mapping &y) {
		using std::swap;
		swap(x._memory, y._memory);
		swap(x._window, y._window);
		swap(x._offset, y._offset);
		swap(x._size, y._size);
	}

	Mapping()
	: _window{nullptr}, _offset{0}, _size{0} { }

	Mapping(helix::UniqueDescriptor memory, ptrdiff_t offset, size_t size)
	: _memory{std::move(memory)}, _offset{offset}, _size{size} {
		HEL_CHECK(helMapMemory(_memory.getHandle(), kHelNullHandle,
				nullptr, _offset & ~(pageSize - 1),
				((_offset & (pageSize - 1)) + _size + (pageSize - 1)) & ~(pageSize - 1),
				kHelMapProtRead | kHelMapProtWrite, &_window));
	}

	Mapping(const Mapping &) = delete;
	
	Mapping(Mapping &&other)
	: Mapping() {
		swap(*this, other);
	}

	~Mapping() {
		if(_window)
			assert(!"Unmap memory here!");
	}

	Mapping &operator= (Mapping other) {
		swap(*this, other);
		return *this;
	}

	helix::BorrowedDescriptor memory() {
		return _memory;
	}
	ptrdiff_t offset() {
		return _offset;
	}

	void *get() {
		return reinterpret_cast<char *>(_window) + (_offset & (pageSize - 1));
	}

private:
	helix::UniqueDescriptor _memory;
	void *_window;
	ptrdiff_t _offset;
	size_t _size;
};

// --------------------------------------------------------
// LegacyPciTransport
// --------------------------------------------------------

namespace {

struct LegacyPciQueue;

struct LegacyPciTransport : Transport {
	friend class LegacyPciQueue;

	LegacyPciTransport(protocols::hw::Device hw_device,
			arch::io_space legacy_space, helix::UniqueDescriptor irq);

	protocols::hw::Device &hwDevice() override {
		return _hwDevice;
	}

	uint32_t loadConfig(arch::scalar_register<uint32_t> offset) override;

	void finalizeFeatures() override;

	void claimQueues(unsigned int max_index) override;
	Queue *setupQueue(unsigned int index) override;

	void runDevice() override;

private:
	cofiber::no_future _processIrqs();

	protocols::hw::Device _hwDevice;
	arch::io_space _legacySpace;
	helix::UniqueDescriptor _irq;

	std::vector<std::unique_ptr<LegacyPciQueue>> _queues;
};

struct LegacyPciQueue : Queue {
	LegacyPciQueue(LegacyPciTransport *transport,
			unsigned int queue_index, size_t queue_size,
			spec::Descriptor *table, spec::AvailableRing *available, spec::UsedRing *used);

protected:
	void notifyTransport() override;

private:
	LegacyPciTransport *_transport;
};

LegacyPciTransport::LegacyPciTransport(protocols::hw::Device hw_device,
		arch::io_space legacy_space, helix::UniqueDescriptor irq)
: _hwDevice{std::move(hw_device)}, _legacySpace{legacy_space},
		_irq{std::move(irq)} { }

uint32_t LegacyPciTransport::loadConfig(arch::scalar_register<uint32_t> r) {
	return _legacySpace.subspace(20).load(r);
}

void LegacyPciTransport::finalizeFeatures() {
	// Does nothing for now.
}

void LegacyPciTransport::claimQueues(unsigned int max_index) {
	_queues.resize(max_index);
}

Queue *LegacyPciTransport::setupQueue(unsigned int queue_index) {
	assert(queue_index < _queues.size());
	assert(!_queues[queue_index]);

	_legacySpace.store(PCI_L_QUEUE_SELECT, queue_index);
	auto queue_size = _legacySpace.load(PCI_L_QUEUE_SIZE);
	assert(queue_size);

	// TODO: Ensure that the queue size is indeed a power of 2.

	// Determine the queue size in bytes.
	constexpr size_t available_align = 2;
	constexpr size_t used_align = 4096;

	auto available_offset = (queue_size * sizeof(spec::Descriptor)
				+ (available_align - 1))
			& ~size_t(available_align - 1);
	auto used_offset = (available_offset + queue_size * sizeof(spec::AvailableRing::Element)
				+ sizeof(spec::AvailableExtra) + (used_align - 1))
			& ~size_t(used_align - 1);

	auto region_size = used_offset + queue_size * sizeof(spec::UsedRing::Element)
				+ sizeof(spec::UsedExtra);

	// Allocate physical memory for the virtq structs.
	assert(region_size < 0x4000); // FIXME: do not hardcode 0x4000
	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x4000, kHelAllocContinuous, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x4000, kHelMapProtRead | kHelMapProtWrite, &window));
	HEL_CHECK(helCloseDescriptor(memory));

	// Setup the memory region.
	auto table = reinterpret_cast<spec::Descriptor *>((char *)window);
	auto available = reinterpret_cast<spec::AvailableRing *>((char *)window + available_offset);
	auto used = reinterpret_cast<spec::UsedRing *>((char *)window + used_offset);
	_queues[queue_index] = std::make_unique<LegacyPciQueue>(this, queue_index, queue_size,
			table, available, used);
	
	// Hand the queue to the device.
	uintptr_t table_physical;
	HEL_CHECK(helPointerPhysical(table, &table_physical));
	_legacySpace.store(PCI_L_QUEUE_ADDRESS, table_physical >> 12);

	return _queues[queue_index].get();
}

void LegacyPciTransport::runDevice() {
	// Negotiate features that we want to use.
	uint32_t negotiated = 0;
	uint32_t offered = _legacySpace.load(PCI_L_DEVICE_FEATURES);
	printf("Features %x\n", offered);

	_legacySpace.store(PCI_L_DRIVER_FEATURES, negotiated);

	// Finally set the DRIVER_OK bit to finish the configuration.
	_legacySpace.store(PCI_L_DEVICE_STATUS, _legacySpace.load(PCI_L_DEVICE_STATUS) | DRIVER_OK);

	_processIrqs();
}

COFIBER_ROUTINE(cofiber::no_future, LegacyPciTransport::_processIrqs(), ([=] {
	COFIBER_AWAIT _hwDevice.enableBusIrq();

	// TODO: The kick here should not be required.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick, 0));

	uint64_t sequence = 0;
	while(true) {
		helix::AwaitEvent await;
		auto &&submit = helix::submitAwaitEvent(_irq, &await, sequence,
				helix::Dispatcher::global());
		COFIBER_AWAIT(submit.async_wait());
		HEL_CHECK(await.error());
		sequence = await.sequence();

		auto isr = _legacySpace.load(PCI_L_ISR_STATUS);

		if(!(isr & 3)) {
			HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckNack, sequence));
			continue;
		}

		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));

		if(isr & 2) {
			std::cout << "core-virtio: Configuration change" << std::endl;
			auto status = _legacySpace.load(PCI_L_DEVICE_STATUS);
			assert(!(status & DEVICE_NEEDS_RESET));
		}
		if(isr & 1)
			for(auto &queue : _queues)
				queue->processInterrupt();
	}
}))

LegacyPciQueue::LegacyPciQueue(LegacyPciTransport *transport,
		unsigned int queue_index, size_t queue_size,
		spec::Descriptor *table, spec::AvailableRing *available, spec::UsedRing *used)
: Queue{queue_index, queue_size, table, available, used}, _transport{transport} { }

void LegacyPciQueue::notifyTransport() {
	_transport->_legacySpace.store(PCI_L_QUEUE_NOTIFY, queueIndex());
}

} // anonymous namespace

// --------------------------------------------------------
// StandardPciTransport
// --------------------------------------------------------

namespace {

struct StandardPciQueue;

struct StandardPciTransport : Transport {
	friend struct StandardPciQueue;

	StandardPciTransport(protocols::hw::Device hw_device,
			Mapping common_mapping, Mapping notify_mapping,
			Mapping isr_mapping, Mapping device_mapping,
			unsigned int notify_multiplier, helix::UniqueDescriptor irq);

	protocols::hw::Device &hwDevice() override {
		return _hwDevice;
	}
	
	uint32_t loadConfig(arch::scalar_register<uint32_t> offset) override;

	bool checkDeviceFeature(unsigned int feature);
	void acknowledgeDriverFeature(unsigned int feature);
	void finalizeFeatures() override;

	void claimQueues(unsigned int max_index) override;
	Queue *setupQueue(unsigned int index) override;

	void runDevice() override;

private:
	arch::mem_space _commonSpace() { return arch::mem_space{_commonMapping.get()}; }
	arch::mem_space _notifySpace() { return arch::mem_space{_notifyMapping.get()}; }
	arch::mem_space _isrSpace() { return arch::mem_space{_isrMapping.get()}; }
	arch::mem_space _deviceSpace() { return arch::mem_space{_deviceMapping.get()}; }

	cofiber::no_future _processIrqs();

	protocols::hw::Device _hwDevice;
	Mapping _commonMapping;
	Mapping _notifyMapping;
	Mapping _isrMapping;
	Mapping _deviceMapping;
	unsigned int _notifyMultiplier;
	helix::UniqueDescriptor _irq;

	std::vector<std::unique_ptr<StandardPciQueue>> _queues;
};

struct StandardPciQueue : Queue {
	StandardPciQueue(StandardPciTransport *transport,
			unsigned int queue_index, size_t queue_size,
			spec::Descriptor *table, spec::AvailableRing *available, spec::UsedRing *used,
			arch::scalar_register<uint16_t> notify_register);

protected:
	void notifyTransport() override;

private:
	StandardPciTransport *_transport;
	arch::scalar_register<uint16_t> _notifyRegister;
};

StandardPciTransport::StandardPciTransport(protocols::hw::Device hw_device,
		Mapping common_mapping, Mapping notify_mapping,
		Mapping isr_mapping, Mapping device_mapping,
		unsigned int notify_multiplier, helix::UniqueDescriptor irq)
: _hwDevice{std::move(hw_device)},
		_commonMapping{std::move(common_mapping)}, _notifyMapping{std::move(notify_mapping)},
		_isrMapping{std::move(isr_mapping)}, _deviceMapping{std::move(device_mapping)},
		_notifyMultiplier{notify_multiplier}, _irq{std::move(irq)} { }

uint32_t StandardPciTransport::loadConfig(arch::scalar_register<uint32_t> r) {
	return _deviceSpace().load(r);
}

bool StandardPciTransport::checkDeviceFeature(unsigned int feature) {
	_commonSpace().store(PCI_DEVICE_FEATURE_SELECT, feature >> 5);
	return _commonSpace().load(PCI_DEVICE_FEATURE_WINDOW) & (uint32_t(1) << (feature & 31));
}

void StandardPciTransport::acknowledgeDriverFeature(unsigned int feature) {
	auto bit = uint32_t(1) << (feature & 31);
	_commonSpace().store(PCI_DRIVER_FEATURE_SELECT, feature >> 5);
	auto current = _commonSpace().load(PCI_DRIVER_FEATURE_WINDOW);
	_commonSpace().store(PCI_DRIVER_FEATURE_WINDOW, current | bit);
}

void StandardPciTransport::finalizeFeatures() {
	assert(checkDeviceFeature(32));
	acknowledgeDriverFeature(32);

	_commonSpace().store(PCI_DEVICE_STATUS, _commonSpace().load(PCI_DEVICE_STATUS) | FEATURES_OK);
	auto confirm = _commonSpace().load(PCI_DEVICE_STATUS);
	assert(confirm & FEATURES_OK);
}

void StandardPciTransport::claimQueues(unsigned int max_index) {
	_queues.resize(max_index);
}

Queue *StandardPciTransport::setupQueue(unsigned int queue_index) {
	assert(queue_index < _queues.size());
	assert(!_queues[queue_index]);

	_commonSpace().store(PCI_QUEUE_SELECT, queue_index);
	auto queue_size = _commonSpace().load(PCI_QUEUE_SIZE);
	auto notify_index = _commonSpace().load(PCI_QUEUE_NOTIFY);
	assert(queue_size);

	// TODO: Ensure that the queue size is indeed a power of 2.

	// Determine the queue size in bytes.
	constexpr size_t available_align = 2;
	constexpr size_t used_align = 4;

	auto available_offset = (queue_size * sizeof(spec::Descriptor)
				+ (available_align - 1))
			& ~size_t(available_align - 1);
	auto used_offset = (available_offset + queue_size * sizeof(spec::AvailableRing::Element)
				+ sizeof(spec::AvailableExtra) + (used_align - 1))
			& ~size_t(used_align - 1);

	auto region_size = used_offset + queue_size * sizeof(spec::UsedRing::Element)
				+ sizeof(spec::UsedExtra);

	// Allocate physical memory for the virtq structs.
	assert(region_size < 0x4000); // FIXME: do not hardcode 0x4000
	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x4000, kHelAllocContinuous, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x4000, kHelMapProtRead | kHelMapProtWrite, &window));
	HEL_CHECK(helCloseDescriptor(memory));

	// Setup the memory region.
	auto table = reinterpret_cast<spec::Descriptor *>((char *)window);
	auto available = reinterpret_cast<spec::AvailableRing *>((char *)window + available_offset);
	auto used = reinterpret_cast<spec::UsedRing *>((char *)window + used_offset);
	_queues[queue_index] = std::make_unique<StandardPciQueue>(this, queue_index, queue_size,
			table, available, used,
			arch::scalar_register<uint16_t>{_notifyMultiplier * notify_index});

	// Hand the queue to the device.
	uintptr_t table_physical, available_physical, used_physical;
	HEL_CHECK(helPointerPhysical(table, &table_physical));
	HEL_CHECK(helPointerPhysical(available, &available_physical));
	HEL_CHECK(helPointerPhysical(used, &used_physical));
	_commonSpace().store(PCI_QUEUE_TABLE[0], table_physical);
	_commonSpace().store(PCI_QUEUE_TABLE[1], table_physical >> 32);
	_commonSpace().store(PCI_QUEUE_AVAILABLE[0], available_physical);
	_commonSpace().store(PCI_QUEUE_AVAILABLE[1], available_physical >> 32);
	_commonSpace().store(PCI_QUEUE_USED[0], used_physical);
	_commonSpace().store(PCI_QUEUE_USED[1], used_physical >> 32);
	_commonSpace().store(PCI_QUEUE_ENABLE, 1);

	return _queues[queue_index].get();
}

void StandardPciTransport::runDevice() {
	// Finally set the DRIVER_OK bit to finish the configuration.
	_commonSpace().store(PCI_DEVICE_STATUS, _commonSpace().load(PCI_DEVICE_STATUS) | DRIVER_OK);

	_processIrqs();
}

COFIBER_ROUTINE(cofiber::no_future, StandardPciTransport::_processIrqs(), ([=] {
	COFIBER_AWAIT connectKernletCompiler();

	std::vector<uint8_t> kernlet_program;
	fnr::emit_to(std::back_inserter(kernlet_program),
		// Load the PCI_ISR register.
		fnr::scope_push{} (
			fnr::intrin{"__mmio_read8", 2, 1} (
				fnr::binding{0}, // IRQ space MMIO region (bound to slot 0).
				fnr::binding{1} // IRQ space MMIO offset (bound to slot 1).
					 + fnr::literal{PCI_ISR.offset()} // Offset of USBSTS.
			) & fnr::literal{3} // Progress and configuration change bits.
		),
		// Ack the IRQ iff one of the bits was set.
		fnr::check_if{},
			fnr::scope_get{0},
		fnr::then{},
			// Trigger the bitset event (bound to slot 2).
			fnr::intrin{"__trigger_bitset", 2, 0} (
				fnr::binding{2},
				fnr::scope_get{0}
			),
			fnr::scope_push{} ( fnr::literal{1} ),
		fnr::else_then{},
			fnr::scope_push{} ( fnr::literal{2} ),
		fnr::end{}
	);

	auto kernlet_object = COFIBER_AWAIT compile(kernlet_program.data(),
			kernlet_program.size(), {BindType::memoryView, BindType::offset,
			BindType::bitsetEvent});

	HelHandle event_handle;
	HEL_CHECK(helCreateBitsetEvent(&event_handle));
	helix::UniqueDescriptor event{event_handle};

	HelKernletData data[3];
	data[0].handle = _isrMapping.memory().getHandle();
	data[1].handle = _isrMapping.offset();
	data[2].handle = event.getHandle();
	HelHandle bound_handle;
	HEL_CHECK(helBindKernlet(kernlet_object.getHandle(), data, 3, &bound_handle));
	HEL_CHECK(helAutomateIrq(_irq.getHandle(), 0, bound_handle));

	COFIBER_AWAIT _hwDevice.enableBusIrq();

	// TODO: The kick here should not be required.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick, 0));

	//std::cout << "core-virtio " << getpid() << ": Starting IRQ loop" << std::endl;
	uint64_t sequence = 0;
	while(true) {
		helix::AwaitEvent await;
		auto &&submit = helix::submitAwaitEvent(event, &await, sequence,
				helix::Dispatcher::global());
		COFIBER_AWAIT(submit.async_wait());
		HEL_CHECK(await.error());
		sequence = await.sequence();
		assert(await.bitset() & 3);
//		std::cout << "core-virtio " << getpid() << ": Got IRQ #" << sequence << std::endl;

		if(await.bitset() & 2) {
			std::cout << "core-virtio: Configuration change" << std::endl;
			auto status = _commonSpace().load(PCI_DEVICE_STATUS);
			assert(!(status & DEVICE_NEEDS_RESET));
		}

		if(await.bitset() & 1)
			for(auto &queue : _queues)
				queue->processInterrupt();
	}
}))

StandardPciQueue::StandardPciQueue(StandardPciTransport *transport,
		unsigned int queue_index, size_t queue_size,
		spec::Descriptor *table, spec::AvailableRing *available, spec::UsedRing *used,
		arch::scalar_register<uint16_t> notify_register)
: Queue{queue_index, queue_size, table, available, used},
		_transport{transport}, _notifyRegister{notify_register} { }

void StandardPciQueue::notifyTransport() {
	_transport->_notifySpace().store(_notifyRegister, queueIndex());
}

} // anonymous namespace

// --------------------------------------------------------
// The discover() function.
// --------------------------------------------------------

COFIBER_ROUTINE(async::result<std::unique_ptr<Transport>>,
discover(protocols::hw::Device hw_device, DiscoverMode mode),
		([hw_device = std::move(hw_device), mode] () mutable {
	auto info = COFIBER_AWAIT hw_device.getPciInfo();
	auto irq = COFIBER_AWAIT hw_device.accessIrq();

	if(mode == DiscoverMode::transitional || mode == DiscoverMode::modernOnly) {
		std::optional<Mapping> common_mapping;
		std::optional<Mapping> notify_mapping;
		std::optional<Mapping> isr_mapping;
		std::optional<Mapping> device_mapping;
		unsigned int notify_multiplier = 0;

		for(size_t i = 0; i < info.caps.size(); i++) {
			if(info.caps[i].type != 0x09)
				continue;

			auto subtype = COFIBER_AWAIT hw_device.loadPciCapability(i, 3, 1);
			if(subtype != 1 && subtype != 2 && subtype != 3 && subtype != 4)
				continue;

			auto bir = COFIBER_AWAIT hw_device.loadPciCapability(i, 4, 1);
			auto offset = COFIBER_AWAIT hw_device.loadPciCapability(i, 8, 4);
			auto length = COFIBER_AWAIT hw_device.loadPciCapability(i, 12, 4);
			std::cout << "virtio: Subtype: " << subtype << ", BAR index: " << bir
					<< ", offset: " << offset << ", length: " << length << std::endl;
		
			assert(info.barInfo[bir].ioType == protocols::hw::IoType::kIoTypeMemory);
			auto bar = COFIBER_AWAIT hw_device.accessBar(bir);
			Mapping mapping{std::move(bar), info.barInfo[bir].offset + offset, length};

			if(subtype == 1) {
				common_mapping = std::move(mapping);
			}else if(subtype == 2) {
				notify_mapping = std::move(mapping);
				notify_multiplier = COFIBER_AWAIT hw_device.loadPciCapability(i, 16, 4);
			}else if(subtype == 3) {
				isr_mapping = std::move(mapping);
			}else if(subtype == 4) {
				device_mapping = std::move(mapping);
			}
		}
		
		if(common_mapping && notify_mapping && isr_mapping && device_mapping) {
			// Reset the device.
			arch::mem_space common_space{common_mapping->get()};
			common_space.store(PCI_DEVICE_STATUS, 0);
			assert(!common_space.load(PCI_DEVICE_STATUS));

			// Set the ACKNOWLEDGE and DRIVER bits.
			// The specification says this should be done in two steps
			common_space.store(PCI_DEVICE_STATUS,
					common_space.load(PCI_DEVICE_STATUS) | ACKNOWLEDGE);
			common_space.store(PCI_DEVICE_STATUS,
					common_space.load(PCI_DEVICE_STATUS) | DRIVER);

			std::cout << "virtio: Using standard PCI transport" << std::endl;
			COFIBER_RETURN(std::make_unique<StandardPciTransport>(std::move(hw_device),
					std::move(*common_mapping), std::move(*notify_mapping),
					std::move(*isr_mapping), std::move(*device_mapping),
					notify_multiplier, std::move(irq)));
		}
	}

	if(mode == DiscoverMode::legacyOnly || mode == DiscoverMode::transitional) {
		if(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort) {
			auto bar = COFIBER_AWAIT hw_device.accessBar(0);
			HEL_CHECK(helEnableIo(bar.getHandle()));
			
			// Reset the device.
			arch::io_space legacy_space{static_cast<uint16_t>(info.barInfo[0].address)};
			legacy_space.store(PCI_L_DEVICE_STATUS, 0);
			assert(!legacy_space.load(PCI_L_DEVICE_STATUS));

			// Set the ACKNOWLEDGE and DRIVER bits.
			// The specification says this should be done in two steps
			legacy_space.store(PCI_L_DEVICE_STATUS,
					legacy_space.load(PCI_L_DEVICE_STATUS) | ACKNOWLEDGE);
			legacy_space.store(PCI_L_DEVICE_STATUS,
					legacy_space.load(PCI_L_DEVICE_STATUS) | DRIVER);

			std::cout << "virtio: Using legacy PCI transport" << std::endl;
			COFIBER_RETURN(std::make_unique<LegacyPciTransport>(std::move(hw_device),
					legacy_space, std::move(irq)));
		}
	}

	throw std::runtime_error("Cannot construct a suitable virtio::Transport");
}))

// --------------------------------------------------------
// Handle
// --------------------------------------------------------

Handle::Handle(Queue *queue, size_t table_index)
: _queue{queue}, _tableIndex{table_index} { }

void Handle::setupBuffer(HostToDeviceType, arch::dma_buffer_view view) {
	assert(view.size());

	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(view.data(), &physical));
	
	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(physical);
	descriptor->length.store(view.size());
}

void Handle::setupBuffer(DeviceToHostType, arch::dma_buffer_view view) {
	assert(view.size());

	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(view.data(), &physical));
	
	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(physical);
	descriptor->length.store(view.size());
	descriptor->flags.store(descriptor->flags.load() | VIRTQ_DESC_F_WRITE);
}

void Handle::setupLink(Handle other) {
	auto descriptor = _queue->_table + _tableIndex;
	descriptor->next.store(other._tableIndex);
	descriptor->flags.store(descriptor->flags.load() | VIRTQ_DESC_F_NEXT);
}

COFIBER_ROUTINE(async::result<void>, scatterGather(HostToDeviceType, Chain &chain, Queue *queue,
		arch::dma_buffer_view view), ([chain = &chain, queue, view] {
	constexpr size_t page_size = 0x1000;
	size_t offset = 0;
	while(offset < view.size()) {
		auto address = reinterpret_cast<uintptr_t>(view.data()) + offset;
		auto chunk = std::min(view.size() - offset, page_size - (address & (page_size - 1)));
		chain->append(COFIBER_AWAIT queue->obtainDescriptor());
		chain->setupBuffer(hostToDevice, view.subview(offset, chunk));
		offset += chunk;
	}
	COFIBER_RETURN();
}))

COFIBER_ROUTINE(async::result<void>, scatterGather(DeviceToHostType, Chain &chain, Queue *queue,
		arch::dma_buffer_view view), ([chain = &chain, queue, view] {
	constexpr size_t page_size = 0x1000;
	size_t offset = 0;
	while(offset < view.size()) {
		auto address = reinterpret_cast<uintptr_t>(view.data()) + offset;
		auto chunk = std::min(view.size() - offset, page_size - (address & (page_size - 1)));
		chain->append(COFIBER_AWAIT queue->obtainDescriptor());
		chain->setupBuffer(deviceToHost, view.subview(offset, chunk));
		offset += chunk;
	}
	COFIBER_RETURN();
}))

// --------------------------------------------------------
// Queue
// --------------------------------------------------------

Queue::Queue(unsigned int queue_index, size_t queue_size, spec::Descriptor *table,
		spec::AvailableRing *available, spec::UsedRing *used)
: _queueIndex{queue_index}, _queueSize{queue_size}, _progressHead{0} {
	// Construct the hardware state.
	_table = new (table) spec::Descriptor[_queueSize];
	_availableRing = new (available) spec::AvailableRing;
	_usedRing = new (used) spec::UsedRing;
	_availableExtra = new (spec::AvailableExtra::get(available, _queueSize)) spec::AvailableExtra;
	_usedExtra = new (spec::UsedExtra::get(used, _queueSize)) spec::UsedExtra;

	// Initializing the table as 0xFFFF helps debugging
	// as qemu complains if it encounters illegal values.

	_availableRing->flags.store(0);
	_availableRing->headIndex.store(0);
	for(size_t i = 0; i < _queueSize; i++)
		_availableRing->elements[i].tableIndex.store(0xFFFF);
	_availableExtra->eventIndex.store(0);

	_usedRing->flags.store(0);
	_usedRing->headIndex.store(0);
	for(size_t i = 0; i < _queueSize; i++)
		_usedRing->elements[i].tableIndex.store(0xFFFF);
	_usedExtra->eventIndex.store(0);
	
	// Construct the software state.
	for(size_t i = 0; i < _queueSize; i++)
		_descriptorStack.push_back(i);
	_activeRequests.resize(_queueSize);
}

COFIBER_ROUTINE(async::result<Handle>, Queue::obtainDescriptor(), ([=] {
	while(true) {
		if(_descriptorStack.empty()) {
			COFIBER_AWAIT _descriptorDoorbell.async_wait();
			continue;
		}

		size_t table_index = _descriptorStack.back();
		_descriptorStack.pop_back();
		
		auto descriptor = _table + table_index;
		descriptor->address.store(0);
		descriptor->length.store(0);
		descriptor->flags.store(0);

		COFIBER_RETURN((Handle{this, table_index}));
	}
}))

void Queue::postDescriptor(Handle handle, Request *request,
		void (*complete)(Request *)) {
	request->complete = complete;

	assert(request);
	assert(!_activeRequests[handle.tableIndex()]);
	_activeRequests[handle.tableIndex()] = request;

	auto enqueue_head = _availableRing->headIndex.load();
	auto ring_index = enqueue_head & (_queueSize - 1);
	_availableRing->elements[ring_index].tableIndex.store(handle.tableIndex());

	asm volatile ( "" : : : "memory" );
	_availableRing->headIndex.store(enqueue_head + 1);
}

void Queue::notify() {
	asm volatile ( "" : : : "memory" );
	if(!(_usedRing->flags.load() & VIRTQ_USED_F_NO_NOTIFY))
		notifyTransport();
}

void Queue::processInterrupt() {
	while(true) {
		auto used_head = _usedRing->headIndex.load();
		// TODO: I think this assertion is incorrect once we issue more than 2^16 requests.
		assert(_progressHead <= used_head);
		if(_progressHead == used_head)
			break;
		
		asm volatile ( "" : : : "memory" );

		auto ring_index = _progressHead & (_queueSize - 1);
		auto table_index = _usedRing->elements[ring_index].tableIndex.load();
		assert(table_index < _queueSize);

		// Dequeue the Request object.
		auto request = _activeRequests[table_index];
		assert(request);
		_activeRequests[table_index] = nullptr;

		// Free all descriptors in the descriptor chain.
		auto chain_index = table_index;
		while(_table[chain_index].flags.load() & VIRTQ_DESC_F_NEXT) {
			auto successor = _table[chain_index].next.load();
			_descriptorStack.push_back(chain_index);
			chain_index = successor;
		}
		_descriptorStack.push_back(chain_index);
		_descriptorDoorbell.ring();

		// Call the completion handler.
		request->complete(request);

		_progressHead++;
	}
}

} // namespace virtio_core

