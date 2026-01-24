// first-party headers
#include <mem/pagemap.h>
#include <mem/vmm.h>
#include <mem/pmm.h>
#include <mem/mmap.h>
#include <panic.h>
#include <klog/klog.h>

// standard headers
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

// Page table operations (map_page, unmap_page, flag_page, virt2pte) require
// the caller to hold pagemap->lock. This protects against:
// 1. Double allocation in get_next_level() when two CPUs map addresses
//    that share intermediate page table levels
// 2. Concurrent modifications to the same page table entries
//
// Exception: The kernel pagemap (g_kernel_pagemap) is initialized single-threaded
// during boot, and kernel mappings are mostly read-only after init.

// tries to find <count> contiguous pages in the virtual memory space
// if it is unable to, it will return 0
// if it finds them, it will return the base address of the first page
uint64_t find_contiguous_pages(pagemap_t* pagemap, size_t count)
{
    for(uint64_t i = 0; i < UINT64_MAX; i += PAGE_SIZE)
    {
        bool found = true;

        for(uint64_t j = 0; j < count; j++)
        {
            uint64_t* pte_p = virt2pte(pagemap, i, false);
            *pte_p &= ~((uint64_t)0xfff);
            if (*pte_p & PTE_FLAG_PRESENT)
            {
                found = false;
                break;
            }
        }

        if(found)
            return i;
    }
    // if we get this far, we have not been able to find any contiguous regions
    return 0;
}

// Maps <count> contiguous pages at <virt_addr> to <phys_addr>
bool map_contiguous_pages(pagemap_t* pagemap, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags, size_t count)
{
    bool success = true;
    for(uint64_t i = 0; i < count; i++)
    {
        // if any pages fail to map, stop mapping and return false
        // note that this means we will end up with a region of memory that is partially mapped
        // which can never be recovered.  This is a serious bug, and should be dealt with.  
        if(!map_page(pagemap, virt_addr + i, phys_addr + i, flags))
        {
            // because rewinding the partially-mapped region is a complex task
            // and this bug is unlikely to happen very often, I'm deferring the
            // job of implementing it for now...
            panic("Kernel bug: implement rewind of partially mapped regions");
            success = false;
            break;
        }
    }
    return success;
}

// Maps a single page from virt_addr to phys_addr with given flags.
bool map_page(pagemap_t* pagemap, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags)
{
    // this is all just magic ways of extracting the page map index from the virtual address
    // don't put too much thought into it, as long as all the magic numbers are correct,
    // the incantation will work.
    uint64_t pml4_entry = (virt_addr & (((uint64_t)0x1ff) << 39)) >> 39;
    uint64_t pml3_entry = (virt_addr & (((uint64_t)0x1ff) << 30)) >> 30;
    uint64_t pml2_entry = (virt_addr & (((uint64_t)0x1ff) << 21)) >> 21;
    uint64_t pml1_entry = (virt_addr & (((uint64_t)0x1ff) << 12)) >> 12;

    uint64_t* pml4 = pagemap->top_level;
    uint64_t* pml3 = get_next_level(pml4, pml4_entry, true);
    if(pml3 == 0) return false;
    uint64_t* pml2 = get_next_level(pml3, pml3_entry, true);
    if(pml2 == 0) return false;
    uint64_t* pml1 = get_next_level(pml2, pml2_entry, true);
    if(pml1 == 0) return false;

    uint64_t* entry = (uint64_t*)(((uint64_t)pml1) + HIGHER_HALF + pml1_entry * 8);
    entry[0] = phys_addr | flags;

    return true;
}

void switch_pagemap(pagemap_t* pagemap)
{
    void* top = pagemap->top_level;
    asm volatile (
        "mov %0, %%cr3"
        : /* no output */
        : "r" (top)
        : "memory"
    );
}

bool virt2phys(pagemap_t* pagemap, uint64_t virt_addr, uint64_t* phys)
{
    uint64_t* pte_p = virt2pte(pagemap, virt_addr, false);
    if((pte_p[0] & 1) == 0)
        return false;
    *phys = pte_p[0] & ~(uint64_t)0xfff;
    return true;
}

// Returns pointer to the page table entry for virt_addr.
// If allocate=true, creates intermediate page tables as needed.
// todo: make this take an uint64_t* argument as the last argument,
// and return a bool (requires updating all call sites though)
uint64_t* virt2pte(pagemap_t* pagemap, uint64_t virt_addr, bool allocate)
{
    uint64_t pml4_entry = (virt_addr & (((uint64_t)0x1ff) << 39)) >> 39;
    uint64_t pml3_entry = (virt_addr & (((uint64_t)0x1ff) << 30)) >> 30;
    uint64_t pml2_entry = (virt_addr & (((uint64_t)0x1ff) << 21)) >> 21;
    uint64_t pml1_entry = (virt_addr & (((uint64_t)0x1ff) << 12)) >> 12;

    uint64_t* pml4 = pagemap->top_level;
    uint64_t* pml3 = get_next_level(pml4, pml4_entry, allocate);
    if(pml3 == 0) return NULL;
    uint64_t* pml2 = get_next_level(pml3, pml3_entry, allocate);
    if(pml2 == 0) return NULL;
    uint64_t* pml1 = get_next_level(pml2, pml2_entry, allocate);
    if(pml1 == 0) return NULL;

    return (uint64_t*) (((uint64_t)&pml1[pml1_entry]) + HIGHER_HALF);
}

// Unmaps a single page at virt.
bool unmap_page(pagemap_t* pagemap, uint64_t virt)
{
    uint64_t* pte_p =  virt2pte(pagemap, virt, false);
    if(pte_p == NULL)
    {
        return false;
    }

    *pte_p = 0;

    uint64_t current_cr3 = read_cr3();
    if(current_cr3 == (uint64_t)pagemap->top_level) {
        invlpg(virt);
    }
    return true;
}

// Updates flags on an existing page mapping.
bool flag_page(pagemap_t* pagemap, uint64_t virt, uint64_t flags)
{
    uint64_t* pte_p =  virt2pte(pagemap, virt, false);
    if(pte_p == NULL)
    {
        return false;
    }

    *pte_p &= ~((uint64_t)0xfff);
    *pte_p |= flags;

    uint64_t current_cr3 = read_cr3();
    if(current_cr3 == (uint64_t)pagemap->top_level) {
        invlpg(virt);
    }
    return true;
}

pagemap_t new_pagemap()
{
    uint64_t* top_level = pmm_alloc(1);
    if (top_level == NULL)
    {
        panic("new_pagemap() allocation failure");
    }

    // import higher half from kernel pagemap
    uint64_t* p1 = (uint64_t*)((uint64_t)top_level + HIGHER_HALF);
    uint64_t* p2 = g_kernel_pagemap.top_level + HIGHER_HALF;

    for(uint64_t i = 256; i < 512; i++)
    {
        p1[i] = p2[i];
    }

    return (pagemap_t){
        .top_level = top_level,
        .mmap_ranges = NULL,
        .mmap_range_count = 0,
        .lock = (lock_t)LOCK_INITIALIZER("pagemap->lock")
    };
}


// Acquires pagemap->lock internally.
// Analyzer can't track lock through pointer.
NO_THREAD_SAFETY_ANALYSIS
bool delete_pagemap(pagemap_t* pagemap)
{
    bool rv = true;
    lock_acquire(&pagemap->lock);

    for(size_t i = 0; i < pagemap->mmap_range_count; i++)
    {
        mmap_range_local_t* local_range = (mmap_range_local_t*) pagemap->mmap_ranges[i];

        if(!munmap(pagemap, local_range->base, local_range->length))
        {
            rv = false;
            break;
        }

    }

    // Release lock BEFORE freeing to avoid use-after-free
    // At this point, no other thread should have a reference to this pagemap
    lock_release(&pagemap->lock);
    free(pagemap);

    return rv;
}
