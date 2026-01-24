#include <mem/vmm.h>
#include <klog/klog.h>
#include <panic.h>
#include <stdint.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <mem/align.h>
#include <mem/pagemap.h>
#include <macro.h>
#include <limine.h>

pagemap_t g_kernel_pagemap;

#define FOUR_GIGS 0x100000000

// Extern symbol defined in linker script
extern char KERNEL_END_SYMBOL[];

// used to determine kernel size
uint64_t get_kernel_end_addr(void) {
    return (uint64_t) KERNEL_END_SYMBOL;
}

uint64_t* get_next_level(uint64_t* current_level, uint64_t index, bool allocate)
{
    uint64_t* ret = 0;
    uint64_t* entry = (uint64_t*)(((uint64_t)current_level) + HIGHER_HALF + index * 8);
    if ((entry[0] & 0x01) != 0)
    {
        // entry is present in page table
        ret = (uint64_t*)(entry[0] & ~((uint64_t)0xfff));
    }
    else
    {
        if (allocate == false)
        {
            return 0;
        }
        ret = pmm_alloc(1);
        if (ret == 0)
        {
            return 0;
        }
        entry[0] = ((uint64_t) ret) | 0b111;
    }

    return ret;
}


// much of this is ported to C from VINIX OS, with adjustments as necessary
// for our purposes.
void vmm_init(uint64_t kernel_base_physical, uint64_t kernel_base_virtual, struct limine_memmap_response* memmap)
{
    klog("vmm", "Kernel physical base: %x", kernel_base_physical);
    klog("vmm", "Kernel base virtual: %x", kernel_base_virtual);

    g_kernel_pagemap = (pagemap_t) {
        .top_level = pmm_alloc(1)
    };
    if (g_kernel_pagemap.top_level == 0) {
        panic("vmm_init: allocation failed");
    }

    for(uint64_t i = 256; i < 512; i++) {
        if(get_next_level(g_kernel_pagemap.top_level, i, true) == 0) {
            panic("vmm init failure");
        }
    }

    // map kernel
    // get the kernel size from the (page-aligned) end address minus the virtual base address
    uint64_t len = align_up(get_kernel_end_addr(), PAGE_SIZE) - kernel_base_virtual;
    klog("vmm", "Kernel ends at %x", get_kernel_end_addr());
    
    klog("vmm", "Mapping 0x%x to 0x%x, length: 0x%x", kernel_base_physical, kernel_base_virtual, len);


    for(uint64_t i = 0; i < len; i += PAGE_SIZE)
    {
        if(!map_page(&g_kernel_pagemap, kernel_base_virtual + i, kernel_base_physical + i, 0x03))
            panic("vmm init failed: unable to map kernel page");
    }
    
    // Map first page only for higher-half (needed for ACPI/BIOS data)
    // We don't identity-map it to still catch NULL pointer dereferences
    if(!map_page(&g_kernel_pagemap, HIGHER_HALF, 0, 0x03))
        panic("vmm init failed: unable to map page 0");

    for(uint64_t i = 0x1000; i < FOUR_GIGS; i += PAGE_SIZE)
    {
        if(!map_page(&g_kernel_pagemap, i, i, 0x03))
            panic("vmm init failed: unable to map kernel page");
        if(!map_page(&g_kernel_pagemap, i + HIGHER_HALF, i, 0x03))
            panic("vmm init failed: unable to map kernel page");
    }

    struct limine_memmap_entry** entries = memmap->entries;

    for(uint64_t i = 0; i < memmap->entry_count; i++)
    {
        uint64_t base = align_down(entries[i]->base, PAGE_SIZE);
        uint64_t top = align_up(entries[i]->base + entries[i]->length, PAGE_SIZE);

        if (top <= ((uint64_t)FOUR_GIGS)) continue;
        for(uint64_t j = base; j < top; j += PAGE_SIZE)
        {
            if (j < ((uint64_t)FOUR_GIGS)) continue;
            if (!map_page(&g_kernel_pagemap, j, j, 0x03))
                panic("vmm init failed: unable to map kernel page");
            if (!map_page(&g_kernel_pagemap, j + HIGHER_HALF, j, 0x03))
                panic("vmm init failed: unable to map kernel page");
        }
    }

    switch_pagemap(&g_kernel_pagemap);

}
