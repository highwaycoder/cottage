#include <klog/klog.h>
#include <string.h>
#include <mem/malloc.h>

#include <lai/host.h>
#include <acpispec/tables.h>

#include <panic.h>
#include <acpi/acpi.h>

#include <io/io.h>
#include <mem/pagemap.h>
#include <mem/pmm.h>

#include <pci/pci.h>

#include <time/timer.h>

extern pagemap_t g_kernel_pagemap;

/* these functions are strongly linked,
    so are 100% needed for the thing to even compile
*/
void *laihost_malloc(size_t len)
{
    return malloc(len);
}

void *laihost_realloc(void *ptr, size_t newsize, size_t oldsize)
{
    if (newsize == oldsize)
    {
        return ptr;
    }
    else if (newsize < oldsize)
    {
        return ptr; // we just waste some RAM in this case
    }
    else
    { // if (newsize > oldsize)
        // allocate a new range that's big enough for the new size
        void *newptr = malloc(newsize);

        // copy anything that was already present
        memcpy(newptr, ptr, oldsize);

        // free the old space
        free(ptr);

        return newptr;
    }
}

void laihost_free(void *ptr, __attribute__((unused)) size_t len)
{
    free(ptr);
}

/*
 * These functions are very important, but weakly linked,
 * so might or might not work as expected
 * */

void laihost_log(int lvl, const char *msg)
{
    switch (lvl)
    {
    case LAI_DEBUG_LOG:
        klog("lai", "[DEBUG] %s", msg);
        break;
    case LAI_WARN_LOG:
        klog("lai", "[WARN] %s", msg);
        break;
    }
}

__attribute__((noreturn)) void laihost_panic(const char *msg)
{
    panic(msg);
}

/**
 * Table functions
 */

void *laihost_scan(const char *sig, size_t index)
{
    if (memcmp(sig, "DSDT", 4) == 0)
    {
        // todo: detect whether we're using XDST or RDST, and use the correct
        // field (x_dsdt or dsdt, respectively) -- for now we just assume XDST
        // because RDST is a pain in the arse
        return (void *)((acpi_fadt_t *)acpi_find_table("FACP", 0))->x_dsdt;
    }
    else
    {
        return acpi_find_table((char *)sig, index);
    }
}

/* Write a byte/word/dword to the given I/O port. */
void laihost_outb(uint16_t port, uint8_t val)
{
    outb(port, val);
}

void laihost_outw(uint16_t port, uint16_t val)
{
    outw(port, val);
}

void laihost_outd(uint16_t port, uint32_t val)
{
    outd(port, val);
}

/* Read a byte/word/dword from the given I/O port. */
uint8_t laihost_inb(uint16_t port)
{
    return inb(port);
}

uint16_t laihost_inw(uint16_t port)
{
    return inw(port);
}

uint32_t laihost_ind(uint16_t port)
{
    return ind(port);
}

void* laihost_map(size_t address, size_t count)
{
    uint64_t virt_addr;

    // todo: there must be a better way of finding a free virtual address than going
    // through every page and checking one by one...
    for(size_t i = 0; i < UINT64_MAX; i += PAGE_SIZE)
    {
        for(uint64_t j = 0; j < count; j+=PAGE_SIZE)
        {
            uint64_t* pte = virt2pte(&g_kernel_pagemap, i + j + HIGHER_HALF, false);
            // If pte is NULL, the page table doesn't exist, so the page isn't present
            if(pte != NULL && (*pte & PTE_FLAG_PRESENT))
            {
                goto _loop_end;
            }
        }
        virt_addr = i + HIGHER_HALF;
        break;
        _loop_end:
        continue;
    }

    for(uint64_t i = 0; i < count; i+=PAGE_SIZE)
    {
        map_page(&g_kernel_pagemap,
                virt_addr + i,   // virt_addr already includes HIGHER_HALF
                address + i,
                PTE_FLAG_PRESENT | PTE_FLAG_WRITABLE);
    }

    return (void*) virt_addr;
}

void laihost_unmap(void* ptr, size_t count)
{
    //bool unmap_page(pagemap_t* pagemap, uint64_t virt)
    uint64_t pages = count / PAGE_SIZE;
    if(count % PAGE_SIZE != 0) pages++;
    for(uint64_t i = 0; i < (pages * PAGE_SIZE); i += PAGE_SIZE)
        unmap_page(&g_kernel_pagemap, (uint64_t)(ptr + i));
}

/* Write a byte/word/dword to the given device's PCI configuration space
   at the given offset. */
void laihost_pci_writeb(__attribute__((unused)) uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset, uint8_t val)
{
    pci_writeb(bus, slot, fun, offset, val);
}

void laihost_pci_writew(__attribute__((unused)) uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset, uint16_t val)
{
    pci_writew(bus, slot, fun, offset, val);
}

void laihost_pci_writed(__attribute__((unused)) uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset, uint32_t val)
{
    pci_writed(bus, slot, fun, offset, val);
}

/* Read a byte/word/dword from the given device's PCI configuration space
   at the given offset. */
uint8_t laihost_pci_readb(__attribute__((unused)) uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset)
{
    return pci_readb(bus, slot, fun, offset);
}

uint16_t laihost_pci_readw(__attribute__((unused)) uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset)
{
    return pci_readw(bus, slot, fun, offset);
}

uint32_t laihost_pci_readd(__attribute__((unused)) uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset)
{
    return pci_readd(bus, slot, fun, offset);
}

void laihost_sleep(uint64_t ms)
{
    sleep(ms);
}

uint64_t laihost_timer()
{
    uint64_t tps = get_ticks_per_second();
    uint64_t ticks = get_ticks();

    // reduce resolution of the timer down to 100ns as this is what LAI expects
    return ticks / (tps * 10000); 
}
