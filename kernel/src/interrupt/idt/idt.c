#include <interrupt/idt.h> 
#include <interrupt/isr.h>
#include <macro.h>
#include <klog/klog.h>
#include <panic.h>
#include <stddef.h>
#include <stdio.h>
#include <mem/pagemap.h>
#include <lock/lock.h>


// the descriptor table
interrupt_descriptor_t idt[IDT_ENTRY_COUNT];

// the interrupt vector table (used by ISRs to look up which vector to call)
void* interrupt_table[IDT_ENTRY_COUNT];

// a simple counter that tracks which interrupt vectors have been assigned
// starts at 32 because the first 32 interrupts are reserved for exceptions,
// and internal stuff
static uint8_t idt_free_vector = 32;

extern void interrupt_service_routines(void);


// the idt descriptor is a pointer to the table,
// including a length parameter
static idtd_t idtd = {
    .limit = sizeof(idt) - 1,
    .ptr = idt,
};


static lock_t idt_lock = LOCK_INITIALIZER("idt_lock");


uint8_t idt_allocate_vector()
{
    lock_acquire(&idt_lock);
    klog("idt", "Allocating vector %x=%d", &idt_free_vector, idt_free_vector);
    // leave the last 16 vectors free for kernel use
    if(idt_free_vector == 0xf0)
    {
        lock_release(&idt_lock);
        panic("IDT exhausted");
        // technically impossible
        return 0;
    }
    uint8_t ret = idt_free_vector++;
    lock_release(&idt_lock);
    return ret;
}

void idt_init()
{
    idt_reload();
}

void idt_reload()
{
    idtd = (idtd_t) {
        .limit = (sizeof(interrupt_descriptor_t) * IDT_ENTRY_COUNT) - 1,
        .ptr = idt
    };

    klog("idt", "IDT at %x", idt);

    asm volatile (
        "lidt %0"
        : : "m" (idtd)
        : "memory"
    );
}

void set_idt_entry(uint16_t idx, uint8_t flags, uint16_t selector, uint8_t ist,
                   void (*handler)())
{
    idt[idx].type_attributes = flags;
    idt[idx].ist = ist;
    idt[idx].selector = selector;
    // convert a 64 bit address into three offset values for the fucky way the IDT
    // works
    idt[idx].offset_low = (uint16_t)((uint64_t)handler);
    idt[idx].offset_mid = (uint16_t)((uint64_t)handler >> 16);
    idt[idx].offset_high = (uint32_t)((uint64_t)handler >> 32);
}

void set_ist(uint16_t vector, uint8_t ist)
{
    idt[vector].ist = ist;
}
