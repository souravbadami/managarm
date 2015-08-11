
#include "kernel.hpp"

enum {
	kPagePresent = 0x1,
	kPageWrite = 0x2,
	kPageUser = 0x4,
	kPageXd = 0x8000000000000000
};

namespace thor {

LazyInitializer<PageSpace> kernelSpace;

void *physicalToVirtual(PhysicalAddr address) {
	return (void *)(0xFFFF800100000000 + address);
}

// --------------------------------------------------------
// PageSpace
// --------------------------------------------------------

PageSpace::PageSpace(PhysicalAddr pml4_address)
			: p_pml4Address(pml4_address) { }

void PageSpace::activate() {
	asm volatile ( "mov %0, %%cr3" : : "r"( p_pml4Address ) : "memory" );
}

PageSpace PageSpace::cloneFromKernelSpace() {
	PhysicalAddr new_pml4_page = physicalAllocator->allocate(1);
	volatile uint64_t *this_pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	volatile uint64_t *new_pml4_pointer = (uint64_t *)physicalToVirtual(new_pml4_page);

	for(int i = 0; i < 256; i++)
		new_pml4_pointer[i] = 0;
	for(int i = 256; i < 512; i++) {
		ASSERT((this_pml4_pointer[i] & kPagePresent) != 0);
		new_pml4_pointer[i] = this_pml4_pointer[i];
	}

	return PageSpace(new_pml4_page);
}

void PageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		bool user_page, uint32_t flags) {
	ASSERT((pointer % 0x1000) == 0);
	ASSERT((physical % 0x1000) == 0);

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);
	
	// the pml4 exists already
	volatile uint64_t *pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);

	// make sure there is a pdpt
	uint64_t pml4_initial_entry = pml4_pointer[pml4_index];
	volatile uint64_t *pdpt_pointer;
	if((pml4_initial_entry & kPagePresent) != 0) {
		pdpt_pointer = (uint64_t *)physicalToVirtual(pml4_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pdpt_page = physicalAllocator->allocate(1);
		pdpt_pointer = (uint64_t *)physicalToVirtual(pdpt_page);
		for(int i = 0; i < 512; i++)
			pdpt_pointer[i] = 0;
		
		uint64_t new_entry = pdpt_page | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		pml4_pointer[pml4_index] = new_entry;
	}
	ASSERT(user_page ? ((pml4_pointer[pml4_index] & kPageUser) != 0)
			: ((pml4_pointer[pml4_index] & kPageUser) == 0));
	
	// make sure there is a pd
	uint64_t pdpt_initial_entry = pdpt_pointer[pdpt_index];
	volatile uint64_t *pd_pointer;
	if((pdpt_initial_entry & kPagePresent) != 0) {
		pd_pointer = (uint64_t *)physicalToVirtual(pdpt_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pd_page = physicalAllocator->allocate(1);
		pd_pointer = (uint64_t *)physicalToVirtual(pd_page);
		for(int i = 0; i < 512; i++)
			pd_pointer[i] = 0;
		
		uint64_t new_entry = pd_page | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		pdpt_pointer[pdpt_index] = new_entry;
	}
	ASSERT(user_page ? ((pdpt_pointer[pdpt_index] & kPageUser) != 0)
			: ((pdpt_pointer[pdpt_index] & kPageUser) == 0));
	
	// make sure there is a pt
	uint64_t pd_initial_entry = pd_pointer[pd_index];
	volatile uint64_t *pt_pointer;
	if((pd_initial_entry & kPagePresent) != 0) {
		pt_pointer = (uint64_t *)physicalToVirtual(pd_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pt_page = physicalAllocator->allocate(1);
		pt_pointer = (uint64_t *)physicalToVirtual(pt_page);
		for(int i = 0; i < 512; i++)
			pt_pointer[i] = 0;
		
		uint64_t new_entry = pt_page | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		pd_pointer[pd_index] = new_entry;
	}
	ASSERT(user_page ? ((pd_pointer[pd_index] & kPageUser) != 0)
			: ((pd_pointer[pd_index] & kPageUser) == 0));
	
	// setup the new pt entry
	ASSERT((pt_pointer[pt_index] & kPagePresent) == 0);
	uint64_t new_entry = physical | kPagePresent;
	if(user_page)
		new_entry |= kPageUser;
	if((flags & kAccessWrite) != 0)
		new_entry |= kPageWrite;
	if((flags & kAccessExecute) == 0)
		new_entry |= kPageXd;
	pt_pointer[pt_index] = new_entry;
}

PhysicalAddr PageSpace::unmapSingle4k(VirtualAddr pointer) {
	ASSERT((pointer % 0x1000) == 0);

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);
	
	// find the pml4_entry
	volatile uint64_t *pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	uint64_t pml4_entry = pml4_pointer[pml4_index];

	// find the pdpt entry
	ASSERT((pml4_entry & kPagePresent) != 0);
	volatile uint64_t *pdpt_pointer
			= (uint64_t *)physicalToVirtual(pml4_entry & 0x000FFFFFFFFFF000);
	uint64_t pdpt_entry = pdpt_pointer[pdpt_index];
	
	// find the pd entry
	ASSERT((pdpt_entry & kPagePresent) != 0);
	volatile uint64_t *pd_pointer
			= (uint64_t *)physicalToVirtual(pdpt_entry & 0x000FFFFFFFFFF000);
	uint64_t pd_entry = pd_pointer[pd_index];
	
	// find the pt entry
	ASSERT((pd_entry & kPagePresent) != 0);
	volatile uint64_t *pt_pointer
			= (uint64_t *)physicalToVirtual(pd_entry & 0x000FFFFFFFFFF000);
	
	// change the pt entry
	ASSERT((pt_pointer[pt_index] & kPagePresent) != 0);
	pt_pointer[pt_index] ^= kPagePresent;

	return pt_pointer[pt_index] & 0x000FFFFFFFFFF000;
}


} // namespace thor

