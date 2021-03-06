
#include <frigg/debug.hpp>
#include <arch/io_space.hpp>
#include "../arch/x86/cpu.hpp"
#include "../arch/x86/hpet.hpp"
#include "../generic/fiber.hpp"
#include "../generic/io.hpp"
#include "../generic/kernel_heap.hpp"
#include "../generic/service_helpers.hpp"
#include "pci/pci.hpp"

#include "fb.hpp"
#include "boot-screen.hpp"

#include <mbus.frigg_pb.hpp>
#include <hw.frigg_pb.hpp>

extern uint8_t fontBitmap[];

namespace thor {

// ------------------------------------------------------------------------
// window handling
// ------------------------------------------------------------------------

constexpr size_t fontHeight = 16;
constexpr size_t fontWidth = 8;

constexpr uint32_t rgb(int r, int g, int b) {
	return (r << 16) | (g << 8) | b;
}

constexpr uint32_t rgbColor[16] = {
	rgb(1, 1, 1),
	rgb(222, 56, 43),
	rgb(57, 181, 74),
	rgb(255, 199, 6),
	rgb(0, 111, 184),
	rgb(118, 38, 113),
	rgb(44, 181, 233),
	rgb(204, 204, 204),
	rgb(128, 128, 128),
	rgb(255, 0, 0),
	rgb(0, 255, 0),
	rgb(255, 255, 0),
	rgb(0, 0, 255),
	rgb(255, 0, 255),
	rgb(0, 255, 255),
	rgb(255, 255, 255) 
};
constexpr uint32_t defaultBg = rgb(16, 16, 16);

struct FbDisplay : TextDisplay {
	FbDisplay(void *ptr, unsigned int width, unsigned int height, size_t pitch)
	: _width{width}, _height{height}, _pitch{pitch / sizeof(uint32_t)} {
		assert(!(pitch % sizeof(uint32_t)));
		setWindow(ptr);
		_clearScreen(defaultBg);
	}

	void setWindow(void *ptr) {
		_window = reinterpret_cast<uint32_t *>(ptr);
	}

	int getWidth() override;
	int getHeight() override;
	
	void setChars(unsigned int x, unsigned int y,
			const char *c, int count, int fg, int bg) override;
	void setBlanks(unsigned int x, unsigned int y, int count, int bg) override;

private:
	void _clearScreen(uint32_t rgb_color);

	volatile uint32_t *_window;
	unsigned int _width;
	unsigned int _height;
	size_t _pitch;
};

int FbDisplay::getWidth() {
	return _width / fontWidth;
}

int FbDisplay::getHeight() {
	return _height / fontHeight;
}

void FbDisplay::setChars(unsigned int x, unsigned int y,
		const char *c, int count, int fg, int bg) {
	auto fg_rgb = rgbColor[fg];
	auto bg_rgb = (bg < 0) ? defaultBg : rgbColor[bg]; 

	auto dest_line = _window + y * fontHeight * _pitch + x * fontWidth;
	for(size_t i = 0; i < fontHeight; i++) {
		auto dest = dest_line;
		for(int k = 0; k < count; k++) {
			auto dc = (c[k] >= 32 && c[k] <= 127) ? c[k] : 127;
			auto fontbits = fontBitmap[(dc - 32) * fontHeight + i];
			for(size_t j = 0; j < fontWidth; j++) {
				int bit = (1 << ((fontWidth - 1) - j));
				*dest++ = (fontbits & bit) ? fg_rgb : bg_rgb;
			}
		}
		dest_line += _pitch;
	}
}

void FbDisplay::setBlanks(unsigned int x, unsigned int y, int count, int bg) {
	auto bg_rgb = (bg < 0) ? defaultBg : rgbColor[bg]; 

	auto dest_line = _window + y * fontHeight * _pitch + x * fontWidth;
	for(size_t i = 0; i < fontHeight; i++) {
		auto dest = dest_line;
		for(int k = 0; k < count; k++) {
			for(size_t j = 0; j < fontWidth; j++)
				*dest++ = bg_rgb;
		}
		dest_line += _pitch;
	}
}

void FbDisplay::_clearScreen(uint32_t rgb_color) {
	auto dest_line = _window;
	for(size_t i = 0; i < _height; i++) {
		auto dest = dest_line;
		for(size_t j = 0; j < _width; j++)
			*dest++ = rgb_color;
		dest_line += _pitch;
	}
}

namespace {
	frigg::LazyInitializer<FbInfo> bootInfo;
	frigg::LazyInitializer<FbDisplay> bootDisplay;
	frigg::LazyInitializer<BootScreen> bootScreen;
}

void initializeBootFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, uint64_t type, void *early_window) {
	bootInfo.initialize();
	auto fb_info = bootInfo.get();
	fb_info->address = address;
	fb_info->pitch = pitch;
	fb_info->width = width;
	fb_info->height = height;
	fb_info->bpp = bpp;
	fb_info->type = type;

	// Initialize the framebuffer with a lower-half window.
	bootDisplay.initialize(early_window,
			fb_info->width, fb_info->height, fb_info->pitch);
	bootScreen.initialize(bootDisplay.get());

	enableLogHandler(bootScreen.get());
}

void transitionBootFb() {
	auto window_size = (bootInfo->height * bootInfo->pitch + (kPageSize - 1)) & ~(kPageSize - 1);
	assert(window_size <= 0x1'000'000);
	auto window = KernelVirtualMemory::global().allocate(0x1'000'000);
	for(size_t pg = 0; pg < window_size; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k(VirtualAddr(window) + pg,
				bootInfo->address + pg, page_access::write, CachingMode::writeCombine);

	// Transition to the kernel mapping window.
	bootDisplay->setWindow(window);
	
	assert(!(bootInfo->address & (kPageSize - 1)));
	bootInfo->memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
			bootInfo->address & ~(kPageSize - 1),
			(bootInfo->height * bootInfo->pitch + (kPageSize - 1)) & ~(kPageSize - 1),
			CachingMode::writeCombine);	

	// Try to attached the framebuffer to a PCI device.
	pci::PciDevice *owner = nullptr;
	for(auto it = pci::allDevices->begin(); it != pci::allDevices->end(); ++it) {
		auto checkBars = [&] () -> bool {
			for(int i = 0; i < 6; i++) {
				if((*it)->bars[i].type != pci::PciDevice::kBarMemory)
					continue;
				// TODO: Careful about overflow here.
				auto bar_begin = (*it)->bars[i].address;
				auto bar_end = (*it)->bars[i].address + (*it)->bars[i].length;
				if(bootInfo->address >= bar_begin
						&& bootInfo->address + bootInfo->height * bootInfo->pitch <= bar_end)
					return true;
			}
			
			return false;
		};

		if(checkBars()) {
			assert(!owner);
			owner = it->get();
		}
	}

	if(!owner)
		frigg::panicLogger() << "thor: Could not find owner for boot framebuffer" << frigg::endLog;
	frigg::infoLogger() << "thor: Boot framebuffer is attached to PCI device "
			<< owner->bus << "." << owner->slot << "." << owner->function << frigg::endLog;
	owner->associatedFrameBuffer = bootInfo.get();
	owner->associatedScreen = bootScreen.get();
}

} // namespace thor

