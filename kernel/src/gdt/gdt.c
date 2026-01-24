#include <gdt/gdt.h>
#include <lock/lock.h>
#include <macro.h>
#include <klog/klog.h>

gdt_entry_t gdt_entries[11];
gdt_pointer_t gdt_pointer;
lock_t gdt_lock;

void gdt_init()
{
    // null descriptor
    gdt_entries[0] = (gdt_entry_t){
        .limit = 0,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 0,
        .granularity = 0,
        .base_high_b = 0,
    };

    // ring 0 16 bit code
    gdt_entries[1] = (gdt_entry_t){
        .limit = 0xffff,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING0 
            | GDT_ACCESS_DESCRIPTOR_TYPE_CODE_OR_DATA 
            | GDT_ACCESS_EXECUTABLE
            | GDT_ACCESS_RW_ENABLE,
        .granularity = 0,
        .base_high_b = 0,
    };

    // ring 0 16 bit data
    gdt_entries[2] = (gdt_entry_t){
        .limit = 0xffff,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING0 
            | GDT_ACCESS_DESCRIPTOR_TYPE_CODE_OR_DATA 
            | GDT_ACCESS_RW_ENABLE,
        .granularity = 0,
        .base_high_b = 0,
    };

    // ring 0 32 bit code
    gdt_entries[3] = (gdt_entry_t){
        .limit = 0xffff,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING0 
            | GDT_ACCESS_EXECUTABLE
            | GDT_ACCESS_DESCRIPTOR_TYPE_CODE_OR_DATA 
            | GDT_ACCESS_RW_ENABLE,
        .granularity = 
          GDT_GRANULARITY_32BIT 
        | GDT_GRANULARITY_LIMIT_PAGES
        | GDT_GRANULARITY_LIMIT_HI,
        .base_high_b = 0,
    };

    // ring 0 32 bit data
    gdt_entries[4] = (gdt_entry_t){
        .limit = 0xffff,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING0 
            | GDT_ACCESS_DESCRIPTOR_TYPE_CODE_OR_DATA 
            | GDT_ACCESS_RW_ENABLE,
        .granularity = 
          GDT_GRANULARITY_32BIT 
        | GDT_GRANULARITY_LIMIT_PAGES
        | GDT_GRANULARITY_LIMIT_HI,
        .base_high_b = 0,
    };
    
    // ring 0 64 bit code
    gdt_entries[5] = (gdt_entry_t){
        .limit = 0x0,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING0 
            | GDT_ACCESS_EXECUTABLE
            | GDT_ACCESS_DESCRIPTOR_TYPE_CODE_OR_DATA 
            | GDT_ACCESS_RW_ENABLE,
        .granularity = 
          GDT_GRANULARITY_64BIT_CODE
        ,
        .base_high_b = 0,
    };

    // ring 0 64 bit data
    gdt_entries[6] = (gdt_entry_t){
        .limit = 0x0,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING0 
            | GDT_ACCESS_DESCRIPTOR_TYPE_CODE_OR_DATA 
            | GDT_ACCESS_RW_ENABLE,
        .granularity = 0,
        .base_high_b = 0,
    };

    // ring 3 64 bit code
    gdt_entries[7] = (gdt_entry_t){
        .limit = 0x0,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING3 
            | GDT_ACCESS_EXECUTABLE
            | GDT_ACCESS_DESCRIPTOR_TYPE_CODE_OR_DATA 
            | GDT_ACCESS_RW_ENABLE,
        .granularity = GDT_GRANULARITY_64BIT_CODE,
        .base_high_b = 0,
    };

    // ring 3 64 bit data
    gdt_entries[8] = (gdt_entry_t){
        .limit = 0x0,
        .base_low_w = 0,
        .base_mid_b = 0,
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING3 
            | GDT_ACCESS_DESCRIPTOR_TYPE_CODE_OR_DATA 
            | GDT_ACCESS_RW_ENABLE,
        .granularity = 0,
        .base_high_b = 0,
    };

    gdt_reload();
}

void gdt_reload()
{
    gdt_pointer = (gdt_pointer_t) {
        .size = sizeof(gdt_entry_t) * 13 - 1,
        .address = &gdt_entries
    };

    uint64_t kernel_code = KERNEL_CODE_SEGMENT;
    uint64_t kernel_data = KERNEL_DATA_SEGMENT;
    uint64_t user_data = USER_DATA_SEGMENT;

    asm volatile (
        "lgdt %0\n"
        "pushq %%rax\n"
        "pushq %1\n"
        "leaq (0x03)(%%rip),  %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "popq %%rax\n"
        "mov %2, %%ds\n"
        "mov %2, %%es\n"
        "mov %2, %%ss\n"
        "mov %3, %%fs\n"
        "mov %3, %%gs\n"
        : :
        "m" (gdt_pointer),
        "r" (kernel_code),
        "r" (kernel_data),
        "r" (user_data)
        : "memory"
    );
}

void gdt_load_tss(void* addr)
{
    lock_acquire(&gdt_lock);
    gdt_entries[9] = (gdt_entry_t){
        .limit = 103,
        .base_low_w = (uint16_t)((uint64_t)addr),
        .base_mid_b = (uint8_t)((uint64_t)addr >> 16),
        .base_high_b = (uint8_t)((uint64_t)addr >> 24),
        .access = 
            GDT_ACCESS_PRESENT 
            | GDT_ACCESS_RING0 
            | GDT_ACCESS_DESCRIPTOR_TYPE_TSS
            | GDT_ACCESS_EXECUTABLE
            | GDT_ACCESS_ACCESSED
            ,
        .granularity = 0,
    };

    // the high part of the GDT TSS entry
    gdt_entries[10] = (gdt_entry_t) {
        .limit = (uint16_t)((uint64_t) addr >> 32),
        .base_low_w = (uint16_t)((uint64_t)addr >> 48)
    };

    asm volatile (
        "ltr %0"
        : : "rm" (TSS_SEGMENT)
        : "memory"
    );

    lock_release(&gdt_lock);
}

