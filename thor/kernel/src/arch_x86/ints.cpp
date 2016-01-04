
#include "../kernel.hpp"

extern "C" void earlyStubDivideByZero();
extern "C" void earlyStubOpcode();
extern "C" void earlyStubDouble();
extern "C" void earlyStubProtection();
extern "C" void earlyStubPage();

extern "C" void faultStubDivideByZero();
extern "C" void faultStubDebug();
extern "C" void faultStubOpcode();
extern "C" void faultStubDouble();
extern "C" void faultStubProtection();
extern "C" void faultStubPage();

extern "C" void thorRtIsrIrq0();
extern "C" void thorRtIsrIrq1();
extern "C" void thorRtIsrIrq2();
extern "C" void thorRtIsrIrq3();
extern "C" void thorRtIsrIrq4();
extern "C" void thorRtIsrIrq5();
extern "C" void thorRtIsrIrq6();
extern "C" void thorRtIsrIrq7();
extern "C" void thorRtIsrIrq8();
extern "C" void thorRtIsrIrq9();
extern "C" void thorRtIsrIrq10();
extern "C" void thorRtIsrIrq11();
extern "C" void thorRtIsrIrq12();
extern "C" void thorRtIsrIrq13();
extern "C" void thorRtIsrIrq14();
extern "C" void thorRtIsrIrq15();

extern "C" void thorRtIsrPreempted();

namespace thor {

uint32_t earlyGdt[3];
uint32_t earlyIdt[512];

extern "C" void handleEarlyDivideByZeroFault(void *rip) {
	frigg::panicLogger.log() << "Division by zero during boot\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

extern "C" void handleEarlyOpcodeFault(void *rip) {
	frigg::panicLogger.log() << "Invalid opcode during boot\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

extern "C" void handleEarlyDoubleFault(uint64_t errcode, void *rip) {
	frigg::panicLogger.log() << "Double fault during boot\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

extern "C" void handleEarlyProtectionFault(uint64_t errcode, void *rip) {
	frigg::panicLogger.log() << "Protection fault during boot\n"
			<< "Segment: " << errcode << "\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

extern "C" void handleEarlyPageFault(uint64_t errcode, void *rip) {
	frigg::panicLogger.log() << "Page fault during boot\n"
			<< "Faulting IP: " << rip << frigg::EndLog();
}

void initializeProcessorEarly() {
	// setup the gdt
	frigg::arch_x86::makeGdtNullSegment(earlyGdt, 0);
	// for simplicity, match the layout with the "real" gdt we load later
	frigg::arch_x86::makeGdtCode64SystemSegment(earlyGdt, 1);
	frigg::arch_x86::makeGdtFlatData32SystemSegment(earlyGdt, 2);

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 3 * 8;
	gdtr.pointer = earlyGdt;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	asm volatile ( "pushq $0x8\n"
			"\rpushq $.L_reloadEarlyCs\n"
			"\rlretq\n"
			".L_reloadEarlyCs:" );
	
	// setup the idt
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 0, 0x8, (void *)&earlyStubDivideByZero, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 0, 0x8, (void *)&earlyStubOpcode, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 8, 0x8, (void *)&earlyStubDouble, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 13, 0x8, (void *)&earlyStubProtection, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(earlyIdt, 14, 0x8, (void *)&earlyStubPage, 0);
	
	frigg::arch_x86::Idtr idtr;
	idtr.limit = 256 * 16;
	idtr.pointer = earlyIdt;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) : "memory" );
}

void setupIdt(uint32_t *table) {
	frigg::arch_x86::makeIdt64IntSystemGate(table, 0,
			0x8, (void *)&faultStubDivideByZero, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 1, 0x8, (void *)&faultStubDebug, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 6,
			0x8, (void *)&faultStubOpcode, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 8,
			0x8, (void *)&faultStubDouble, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 13,
			0x8, (void *)&faultStubProtection, 0);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 14,
			0x8, (void *)&faultStubPage, 0);

	frigg::arch_x86::makeIdt64IntSystemGate(table, 64,
			0x8, (void *)&thorRtIsrIrq0, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 65,
			0x8, (void *)&thorRtIsrIrq1, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 66,
			0x8, (void *)&thorRtIsrIrq2, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 67,
			0x8, (void *)&thorRtIsrIrq3, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 68,
			0x8, (void *)&thorRtIsrIrq4, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 69,
			0x8, (void *)&thorRtIsrIrq5, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 70,
			0x8, (void *)&thorRtIsrIrq6, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 71,
			0x8, (void *)&thorRtIsrIrq7, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 72,
			0x8, (void *)&thorRtIsrIrq8, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 73,
			0x8, (void *)&thorRtIsrIrq9, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 74,
			0x8, (void *)&thorRtIsrIrq10, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 75,
			0x8, (void *)&thorRtIsrIrq11, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 76,
			0x8, (void *)&thorRtIsrIrq12, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 77,
			0x8, (void *)&thorRtIsrIrq13, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 78,
			0x8, (void *)&thorRtIsrIrq14, 1);
	frigg::arch_x86::makeIdt64IntSystemGate(table, 79,
			0x8, (void *)&thorRtIsrIrq15, 1);
	
	frigg::arch_x86::makeIdt64IntSystemGate(table, 0x82,
			0x8, (void *)&thorRtIsrPreempted, 0);
}

bool intsAreEnabled() {
	uint64_t rflags;
	asm volatile ( "pushfq\n"
			"\rpop %0" : "=r" (rflags) );
	return (rflags & 0x200) != 0;
}

void enableInts() {
	asm volatile ( "sti" );
}

void disableInts() {
	asm volatile ( "cli" );
}

} // namespace thor

