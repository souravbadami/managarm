
#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>

#include "generic/kernel.hpp"
#include "generic/service_helpers.hpp"

namespace thor {

namespace {
	constexpr bool disableSmp = false;
}

// --------------------------------------------------------
// Debugging functions
// --------------------------------------------------------

int x = 0;
int y = 0;

void advanceY() {
	y++;
	x = 0;
	if(y >= 25) {
//		if(haveTimer())
//			pollSleepNano(100000000);

		PageAccessor accessor{0xB8000};
		auto base = (volatile char *)accessor.get();
		for(auto i = 0; i < 24; i++) {
			for(auto j = 0; j < 80; j++) {
				base[(80 * i + j) * 2] = base[(80 * (i + 1) + j) * 2];
				base[(80 * i + j) * 2 + 1] = base[(80 * (i + 1) + j) * 2 + 1];
			}
		}
		for(auto j = 0; j < 80; j++) {
			base[(80 * 24 + j) * 2] = ' ';
			base[(80 * 24 + j) * 2 + 1] = 0x0F;
		}
		y = 24;
	}
}

arch::scalar_register<uint8_t> data(0);
arch::scalar_register<uint8_t> baudLow(0);
arch::scalar_register<uint8_t> baudHigh(1);
arch::bit_register<uint8_t> lineControl(3);
arch::bit_register<uint8_t> lineStatus(5);

arch::field<uint8_t, bool> txReady(5, 1);

arch::field<uint8_t, int> dataBits(0, 2);
arch::field<uint8_t, bool> stopBit(2, 1);
arch::field<uint8_t, int> parityBits(3, 3);
arch::field<uint8_t, bool> dlab(7, 1);

extern bool debugToVga;
extern bool debugToSerial;
extern bool debugToBochs;

struct LogMessage {
	char text[100];
};

size_t currentLogLength;
LogMessage logQueue[1024];
size_t logHead;

size_t currentLogSequence() {
	return logHead;
}

void copyLogMessage(size_t sequence, char *text) {
	memcpy(text, logQueue[sequence % 1024].text, 100);
}

frigg::LazyInitializer<frg::intrusive_list<
	LogHandler,
	frg::locate_member<
		LogHandler,
		frg::default_list_hook<LogHandler>,
		&LogHandler::hook
	>
>> globalLogList;

void setupDebugging() {
	if(debugToSerial) {
		auto base = arch::global_io.subspace(0x3F8);
		
		// Set the baud rate.
		base.store(lineControl, dlab(true));
		base.store(baudLow, 0x01);
		base.store(baudHigh, 0x00);

		// Configure: 8 data bits, 1 stop bit, no parity.
		base.store(lineControl, dataBits(3) | stopBit(0) | parityBits(0) | dlab(false));
	}

	globalLogList.initialize();
}

void enableLogHandler(LogHandler *sink) {
	globalLogList->push_back(sink);
}

void disableLogHandler(LogHandler *sink) {
	auto it = globalLogList->iterator_to(sink);
	globalLogList->erase(it);
}

namespace {

void callLegacy(char c) {
	// --------------------------------------------------------
	// Text-mode video output
	// --------------------------------------------------------
	
	if(debugToVga) {
		if(c == '\n') {
			advanceY();
		}else{
			PageAccessor accessor{0xB8000};
			auto base = (volatile char *)accessor.get();

			base[(80 * y + x) * 2] = c;
			base[(80 * y + x) * 2 + 1] = 0x0F;

			x++;
			if(x >= 80) {
				advanceY();
			}
		}
	}

	// --------------------------------------------------------
	// Serial console
	// --------------------------------------------------------
	if(debugToSerial) {
		auto base = arch::global_io.subspace(0x3F8);

		if(c == '\n') {
			while(!(base.load(lineStatus) & txReady)) {
				// do nothing until the UART is ready to transmit.
			}
			base.store(data, '\r');
		}

		while(!(base.load(lineStatus) & txReady)) {
			// do nothing until the UART is ready to transmit.
		}
		base.store(data, c);
	}

	// --------------------------------------------------------
	// Bochs/Qemu debugging port
	// --------------------------------------------------------
	if(debugToBochs)
		frigg::arch_x86::ioOutByte(0xE9, c);
}

} // anonymous namespace

constexpr int maximalCsiLength = 16;
char csiBuffer[maximalCsiLength];
int csiState;
int csiLength;

void BochsSink::print(char c) {
	auto doesFit = [] (int n) -> bool {
		return currentLogLength + n < 100;
	};

	auto cutOff = [] () {
		currentLogLength = 0;
		logHead++;
		memset(logQueue[logHead % 1024].text, 0, 100);
		callLegacy('\n');
	};

	auto emit = [] (char c) {
		logQueue[logHead % 1024].text[currentLogLength] = c;
		currentLogLength++;
		callLegacy(c);
	};

	if(!csiState) {
		if(c == '\x1B') {
			csiState = 1;
		}else if(c == '\n' || !doesFit(1)) {
			cutOff();
		}else{
			emit(c);
		}
	}else if(csiState == 1) {
		if(c == '[') {
			csiState = 2;
		}else{
			if(!doesFit(2)) {
				cutOff();
			}else{
				emit('\x1B');
				emit(c);
			}
			csiState = 0;
		}
	}else{
		// This is csiState == 2.
		if((c >= '0' && c <= '9') || (c == ';')) {
			if(csiLength < maximalCsiLength)
				csiBuffer[csiLength] = c;
			csiLength++;
		}else{
			if(csiLength >= maximalCsiLength || !doesFit(3 + csiLength)) {
				cutOff();
			}else{
				emit('\x1B');
				emit('[');
				for(int i = 0; i < csiLength; i++)
					emit(csiBuffer[i]);
				emit(c);
			}
			csiState = 0;
			csiLength = 0;
		}
	}

	for(auto it = globalLogList->begin(); it != globalLogList->end(); ++it) {
		(*it)->printChar(c);
	}
}

void BochsSink::print(const char *str) {
	while(*str != 0)
		print(*str++);
}

// --------------------------------------------------------

namespace {
	void activateTss(frigg::arch_x86::Tss64 *tss) {
		frigg::arch_x86::makeGdtTss64Descriptor(getCpuData()->gdt, kGdtIndexTask,
				tss, sizeof(frigg::arch_x86::Tss64));
		asm volatile ("ltr %w0" : : "r"(kSelTask) : "memory");
	}
}

// --------------------------------------------------------
// UniqueKernelStack
// --------------------------------------------------------

UniqueKernelStack UniqueKernelStack::make() {
	auto pointer = (char *)kernelAlloc->allocate(kSize);
	return UniqueKernelStack(pointer + kSize);
}

UniqueKernelStack::~UniqueKernelStack() {
	if(_base)
		kernelAlloc->free(_base - kSize);
}

// --------------------------------------------------------
// FaultImageAccessor
// --------------------------------------------------------

bool FaultImageAccessor::allowUserPages() {
	assert(inKernelDomain());
	if(!getCpuData()->haveSmap)
		return true;
	return *rflags() & (uint32_t(1) << 18);
}

// --------------------------------------------------------
// Executor
// --------------------------------------------------------

size_t Executor::determineSize() {
	return sizeof(General) + sizeof(FxState);
}

Executor::Executor()
: _pointer{nullptr}, _syscallStack{nullptr}, _tss{nullptr} { }

Executor::Executor(UserContext *context, AbiParameters abi) {
	_pointer = (char *)kernelAlloc->allocate(getStateSize());
	memset(_pointer, 0, getStateSize());

	_fxState()->mxcsr |= 1 << 7;
	_fxState()->mxcsr |= 1 << 8;
	_fxState()->mxcsr |= 1 << 9;
	_fxState()->mxcsr |= 1 << 10;
	_fxState()->mxcsr |= 1 << 11;
	_fxState()->mxcsr |= 1 << 12;

	general()->rip = abi.ip;
	general()->rflags = 0x200;
	general()->rsp = abi.sp;
	general()->cs = kSelClientUserCode;
	general()->ss = kSelClientUserData;

	_tss = &context->tss;
	_syscallStack = context->kernelStack.base();
}

Executor::Executor(FiberContext *context, AbiParameters abi)
: _syscallStack{nullptr}, _tss{nullptr} {
	_pointer = (char *)kernelAlloc->allocate(getStateSize());
	memset(_pointer, 0, getStateSize());

	_fxState()->mxcsr |= 1 << 7;
	_fxState()->mxcsr |= 1 << 8;
	_fxState()->mxcsr |= 1 << 9;
	_fxState()->mxcsr |= 1 << 10;
	_fxState()->mxcsr |= 1 << 11;
	_fxState()->mxcsr |= 1 << 12;

	general()->rip = abi.ip;
	general()->rflags = 0x200;
	general()->rsp = (uintptr_t)context->stack.base();
	general()->rdi = abi.argument;
	general()->cs = kSelSystemFiberCode;
	general()->ss = kSelExecutorKernelData;
}

Executor::~Executor() {
	kernelAlloc->free(_pointer);
}

void saveExecutor(Executor *executor, FaultImageAccessor accessor) {
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rbx = accessor._frame()->rbx;
	executor->general()->rcx = accessor._frame()->rcx;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r11 = accessor._frame()->r11;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;
	
	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = accessor._frame()->cs;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = accessor._frame()->ss;
	executor->general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	executor->general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);
	
	asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
}

void saveExecutor(Executor *executor, IrqImageAccessor accessor) {
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rbx = accessor._frame()->rbx;
	executor->general()->rcx = accessor._frame()->rcx;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r11 = accessor._frame()->r11;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;
	
	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = accessor._frame()->cs;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = accessor._frame()->ss;
	executor->general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	executor->general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);
	
	asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
}

void saveExecutor(Executor *executor, SyscallImageAccessor accessor) {
	// Note that rbx, rcx and r11 are used internally by the syscall mechanism.
	executor->general()->rax = accessor._frame()->rax;
	executor->general()->rdx = accessor._frame()->rdx;
	executor->general()->rdi = accessor._frame()->rdi;
	executor->general()->rsi = accessor._frame()->rsi;
	executor->general()->rbp = accessor._frame()->rbp;

	executor->general()->r8 = accessor._frame()->r8;
	executor->general()->r9 = accessor._frame()->r9;
	executor->general()->r10 = accessor._frame()->r10;
	executor->general()->r12 = accessor._frame()->r12;
	executor->general()->r13 = accessor._frame()->r13;
	executor->general()->r14 = accessor._frame()->r14;
	executor->general()->r15 = accessor._frame()->r15;

	// Note that we do not save cs and ss on syscall.
	// We just assume that these registers have their usual values.
	executor->general()->rip = accessor._frame()->rip;
	executor->general()->cs = kSelClientUserCode;
	executor->general()->rflags = accessor._frame()->rflags;
	executor->general()->rsp = accessor._frame()->rsp;
	executor->general()->ss = kSelClientUserData;
	executor->general()->clientFs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexFsBase);
	executor->general()->clientGs = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrIndexKernelGsBase);
	
	asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
}

void switchExecutor(frigg::UnsafePtr<Thread> thread) {
	assert(!intsAreEnabled());
	getCpuData()->activeExecutor = thread;
}

extern "C" void workStub();

void workOnExecutor(Executor *executor) {
	auto nsp = reinterpret_cast<uint64_t *>(executor->getSyscallStack());

	auto push = [&] (uint64_t v) {
		memcpy(--nsp, &v, sizeof(uint64_t));
	};

	// Build an IRET frame on the syscall stack.
	push(*executor->ss());
	push(*executor->sp());
	push(*executor->rflags());
	push(*executor->cs());
	push(*executor->ip());

	// Point the executor to the work stub.
	void *stub = reinterpret_cast<void *>(&workStub);
	*executor->ip() = reinterpret_cast<uintptr_t>(stub);
	*executor->cs() = kSelExecutorSyscallCode;
	*executor->rflags() &= ~uint64_t(0x200); // Disable IRQs.
	*executor->sp() = reinterpret_cast<uintptr_t>(nsp);
	*executor->ss() = 0;
}

extern "C" [[ noreturn ]] void _restoreExecutorRegisters(void *pointer);

[[ gnu::section(".text.stubs") ]] void restoreExecutor(Executor *executor) {
	if(executor->_tss) {
		activateTss(executor->_tss);
	}else{
		activateTss(&getCpuData()->tss);
	}

	getCpuData()->syscallStack = executor->_syscallStack;

	// TODO: use wr{fs,gs}base if it is available
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, executor->general()->clientFs);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexKernelGsBase, executor->general()->clientGs);

	uint16_t cs = executor->general()->cs;
	assert(cs == kSelExecutorFaultCode || cs == kSelExecutorSyscallCode
			|| cs == kSelClientUserCode || cs == kSelSystemFiberCode);
	if(cs == kSelClientUserCode)
		asm volatile ( "swapgs" : : : "memory" );

	_restoreExecutorRegisters(executor->general());
}

// --------------------------------------------------------
// UserContext
// --------------------------------------------------------

void UserContext::deactivate() {
	activateTss(&getCpuData()->tss);
}

UserContext::UserContext()
: kernelStack{UniqueKernelStack::make()} {
	memset(&tss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&tss);
	tss.rsp0 = (Word)kernelStack.base();
}

void UserContext::enableIoPort(uintptr_t port) {
	tss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

void UserContext::migrate(CpuData *cpu_data) {
	assert(!intsAreEnabled());
	tss.ist1 = (Word)cpu_data->irqStack.base();
	tss.ist2 = (Word)cpu_data->nmiStack.base();
}

frigg::UnsafePtr<Thread> activeExecutor() {
	return getCpuData()->activeExecutor;
}

// --------------------------------------------------------
// FiberContext
// --------------------------------------------------------

FiberContext::FiberContext(UniqueKernelStack stack)
: stack{std::move(stack)} { }

// --------------------------------------------------------
// PlatformCpuData
// --------------------------------------------------------

PlatformCpuData::PlatformCpuData()
: haveSmap{false}, havePcids{false} {
	for(int i = 0; i < maxPcidCount; i++)
		pcidBindings[i].setupPcid(i);

	// Setup the GDT.
	// Note: the TSS requires two slots in the GDT.
	frigg::arch_x86::makeGdtNullSegment(gdt, kGdtIndexNull);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexInitialCode);

	frigg::arch_x86::makeGdtTss64Descriptor(gdt, kGdtIndexTask, nullptr, 0);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemIrqCode);

	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexExecutorFaultCode);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexExecutorSyscallCode);
	frigg::arch_x86::makeGdtFlatData32SystemSegment(gdt, kGdtIndexExecutorKernelData);
	frigg::arch_x86::makeGdtNullSegment(gdt, kGdtIndexClientUserCompat);
	frigg::arch_x86::makeGdtFlatData32UserSegment(gdt, kGdtIndexClientUserData);
	frigg::arch_x86::makeGdtCode64UserSegment(gdt, kGdtIndexClientUserCode);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemIdleCode);
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemFiberCode);
	
	frigg::arch_x86::makeGdtCode64SystemSegment(gdt, kGdtIndexSystemNmiCode);
	
	// Setup the per-CPU TSS. This TSS is used by system code.
	memset(&tss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&tss);
}

void enableUserAccess() {
	if(getCpuData()->haveSmap)
		asm volatile ("stac" : : : "memory");
}
void disableUserAccess() {
	if(getCpuData()->haveSmap)
		asm volatile ("clac" : : : "memory");
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

namespace {
	frigg::LazyInitializer<frigg::Vector<CpuData *, KernelAlloc>> allCpuContexts;
}

size_t getStateSize() {
	return Executor::determineSize();
}

CpuData *getCpuData() {
	AssemblyCpuData *cpu_data;
	asm ("mov %%gs:0, %0" : "=r"(cpu_data));
	return static_cast<CpuData *>(cpu_data);
}

CpuData *getCpuData(size_t k) {
	return (*allCpuContexts)[k];
}

int getCpuCount() {
	return allCpuContexts->size();
}

void doRunDetached(void (*function) (void *), void *argument) {
	assert(!intsAreEnabled());

	CpuData *cpu_data = getCpuData();
	
	uintptr_t stack_ptr = (uintptr_t)cpu_data->detachedStack.base();
	asm volatile ( "mov %%rsp, %%rbp\n"
			"\tmov %2, %%rsp\n"
			"\tcall *%1\n"
			"\tmov %%rbp, %%rsp" : : "D" (argument), "r" (function), "r" (stack_ptr) : "rbp" );
}

extern "C" void syscallStub();

frigg::LazyInitializer<CpuData> staticBootCpuContext;

// Set up the kernel GS segment.
void setupCpuContext(AssemblyCpuData *context) {
	context->selfPointer = context;
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexGsBase,
			reinterpret_cast<uint64_t>(context));
}

void earlyInitializeBootProcessor() {
	staticBootCpuContext.initialize();
	setupCpuContext(staticBootCpuContext.get());
}

void initializeBootProcessor() {
	allCpuContexts.initialize(*kernelAlloc);

	// We need to fill in the boot APIC ID.
	// This cannot be done in earlyInitializeBootProcessor() as we need the APIC base first.
	staticBootCpuContext->localApicId = getLocalApicId();
}

void initializeThisProcessor() {	
	// FIXME: the stateSize should not be CPU specific!
	// move it to a global variable and initialize it in initializeTheSystem() etc.!
	auto cpu_data = getCpuData();
	
	// TODO: If we want to make bootSecondary() parallel, we have to lock here.
	allCpuContexts->push(cpu_data);

	// Allocate per-CPU areas.
	cpu_data->irqStack = UniqueKernelStack::make();
	cpu_data->nmiStack = UniqueKernelStack::make();
	cpu_data->detachedStack = UniqueKernelStack::make();

	// We embed some data at the top of the NMI stack.
	// The NMI handler needs this data to enter a consistent kernel state.
	struct Embedded {
		AssemblyCpuData *expectedGs;
		uint64_t padding;
	} embedded{cpu_data, 0};

	cpu_data->nmiStack.embed<Embedded>(embedded);

	// Setup our IST after the did the embedding.
	cpu_data->tss.ist1 = (uintptr_t)cpu_data->irqStack.base();
	cpu_data->tss.ist2 = (uintptr_t)cpu_data->nmiStack.base();

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 14 * 8;
	gdtr.pointer = cpu_data->gdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq %0\n"
			"\rpushq $.L_reloadCs\n"
			"\rlretq\n"
			".L_reloadCs:" : : "i" (kSelInitialCode) );

	// We need a valid TSS in case an NMI or fault happens here.
	activateTss(&cpu_data->tss);
	
	// setup the idt
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate(cpu_data->idt, i);
	setupIdt(cpu_data->idt);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = cpu_data->idt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) );

	// Enable the global page feature.
	{
		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 7;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));
	}

	// Enable the wr{fs,gs}base instructions.
	// FIXME: does not seem to work under qemu
//	if(!(frigg::arch_x86::cpuid(frigg::arch_x86::kCpuIndexStructuredExtendedFeaturesEnum)[1]
//			& frigg::arch_x86::kCpuFlagFsGsBase))
//		frigg::panicLogger() << "CPU does not support wrfsbase / wrgsbase"
//				<< frigg::endLog;
	
//	uint64_t cr4;
//	asm volatile ( "mov %%cr4, %0" : "=r" (cr4) );
//	cr4 |= 0x10000;
//	asm volatile ( "mov %0, %%cr4" : : "r" (cr4) );

	// Enable the SMAP extension.
	if(frigg::arch_x86::cpuid(0x07)[1] & (uint32_t(1) << 20)) {
		frigg::infoLogger() << "\e[37mthor: CPU supports SMAP\e[39m" << frigg::endLog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 21;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		asm volatile ("clac" : : : "memory");

		cpu_data->haveSmap = true;
	}else{
		frigg::infoLogger() << "\e[37mthor: CPU does not support SMAP!\e[39m" << frigg::endLog;
	}

	// Enable the PCID extension.
	if(frigg::arch_x86::cpuid(0x01)[2] & (uint32_t(1) << 17)) {
		frigg::infoLogger() << "\e[37mthor: CPU supports PCIDs\e[39m" << frigg::endLog;

		if(!(frigg::arch_x86::cpuid(0x07)[1] & (uint32_t(1) << 10)))
			frigg::panicLogger() << "\e[31mthor: However, INVPCID is not supported!\e[39m"
					<< frigg::endLog;

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= uint32_t(1) << 17;
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		cpu_data->havePcids = true;
	}else{
		frigg::infoLogger() << "\e[37mthor: CPU does not support PCIDs!\e[39m" << frigg::endLog;
	}

	// setup the syscall interface
	if((frigg::arch_x86::cpuid(frigg::arch_x86::kCpuIndexExtendedFeatures)[3]
			& frigg::arch_x86::kCpuFlagSyscall) == 0)
		frigg::panicLogger() << "CPU does not support the syscall instruction"
				<< frigg::endLog;
	
	uint64_t efer = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrEfer);
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrEfer,
			efer | frigg::arch_x86::kMsrSyscallEnable);

	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrLstar, (uintptr_t)&syscallStub);
	// set user mode rpl bits to work around a qemu bug
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrStar, (uint64_t(kSelClientUserCompat) << 48)
			| (uint64_t(kSelExecutorSyscallCode) << 32));
	// mask interrupt and trap flag
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrFmask, 0x300);

	initLocalApicPerCpu();
}

// Generated by objcopy.
extern "C" uint8_t _binary_thor_kernel_trampoline_bin_start[];
extern "C" uint8_t _binary_thor_kernel_trampoline_bin_end[];

struct StatusBlock {
	unsigned int targetStage;
	unsigned int initiatorStage;
	unsigned int pml4;
	uintptr_t stack;
	void (*main)(StatusBlock *);
	AssemblyCpuData *cpuContext;
};

static_assert(sizeof(StatusBlock) == 40, "Bad sizeof(StatusBlock)");

void secondaryMain(StatusBlock *status_block) {
	setupCpuContext(status_block->cpuContext);
	initializeThisProcessor();
	__atomic_store_n(&status_block->targetStage, 2, __ATOMIC_RELEASE);

	frigg::infoLogger() << "Hello world from CPU #" << getLocalApicId() << frigg::endLog;	
	localScheduler()->reschedule();
}

void bootSecondary(unsigned int apic_id) {
	if(disableSmp)
		return;

	// TODO: Allocate a page in low physical memory instead of hard-coding it.
	uintptr_t pma = 0x10000;

	// Copy the trampoline code into low physical memory.
	auto image_size = (uintptr_t)_binary_thor_kernel_trampoline_bin_end
			- (uintptr_t)_binary_thor_kernel_trampoline_bin_start;
	assert(image_size <= kPageSize);
	PageAccessor accessor{pma};
	memcpy(accessor.get(), _binary_thor_kernel_trampoline_bin_start, image_size);
	
	// Allocate a stack for the initialization code.
	constexpr size_t stack_size = 0x10000;
	void *stack_ptr = kernelAlloc->allocate(stack_size);
	
	auto context = frigg::construct<CpuData>(*kernelAlloc);
	context->localApicId = apic_id;

	// Setup a status block to communicate information to the AP.
	auto status_block = reinterpret_cast<StatusBlock *>(reinterpret_cast<char *>(accessor.get())
			+ (kPageSize - sizeof(StatusBlock)));

	status_block->targetStage = 0;
	status_block->initiatorStage = 0;
	status_block->pml4 = KernelPageSpace::global().rootTable();
	status_block->stack = (uintptr_t)stack_ptr + stack_size;
	status_block->main = &secondaryMain;
	status_block->cpuContext = context;

	// Send the IPI sequence that starts up the AP.
	// On modern processors INIT lets the processor enter the wait-for-SIPI state.
	// The BIOS is not involved in this process at all.
	frigg::infoLogger() << "thor: Booting AP " << apic_id << "." << frigg::endLog;
	raiseInitAssertIpi(apic_id);
	fiberSleep(10000000); // Wait for 10ms.

	// SIPI causes the processor to resume execution and resets CS:IP.
	// Intel suggets to send two SIPIs (probably for redundancy reasons).
	raiseStartupIpi(apic_id, pma);
	fiberSleep(200000); // Wait for 200us.
	raiseStartupIpi(apic_id, pma);
	fiberSleep(200000); // Wait for 200us.
	
	// Wait until the AP wakes up.
	while(__atomic_load_n(&status_block->targetStage, __ATOMIC_ACQUIRE) < 1) {
		frigg::pause();
	}
	frigg::infoLogger() << "thor: AP did wake up." << frigg::endLog;

	// We only let the AP proceed after all IPIs have been sent.
	// This ensures that the AP does not execute boot code twice (e.g. in case
	// it already wakes up after a single SIPI).
	__atomic_store_n(&status_block->initiatorStage, 1, __ATOMIC_RELEASE);
	
	// Wait until the AP exits the boot code.
	while(__atomic_load_n(&status_block->targetStage, __ATOMIC_ACQUIRE) < 2) {
		frigg::pause();
	}
	frigg::infoLogger() << "thor: AP finished booting." << frigg::endLog;
}

} // namespace thor

