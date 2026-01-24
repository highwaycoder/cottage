#include <interrupt/isr.h>
#include <stdint.h>
#include <stddef.h>
#include <interrupt/idt.h>
#include <interrupt/apic.h>
#include <gdt/gdt.h>
#include <klog/klog.h>
#include <panic.h>

// names of each type of exception the CPU can produce
static const char *exception_names[] = {
    "Divide by Zero Error",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Security Exception",
    "Reserved"};

void isr_initialize_gates();

extern void interrupt_service_routines(void);

uint64_t abort_vector;

void handle_pagefault(__attribute__((unused)) uint32_t num, __attribute__((unused)) cpu_status_t* cpu_state)
{
    klog("isr", "Handling pagefault");
    void* faulting_address = NULL;
    asm volatile("mov %%cr2, %0" : "=r" (faulting_address));
    klog("isr", "cr2=%lp rip=%lp", faulting_address, cpu_state->rip);
    panic("Can't handle PF properly yet");
}

void handle_abort(__attribute__((unused)) uint32_t num, __attribute__((unused)) cpu_status_t* cpu_state)
{
    atomic_store(&cpu_get_current()->aborted, true);
    for(;;)
    {
        asm volatile("hlt");
    }
}

void handle_exception(uint32_t num, cpu_status_t* cpu_state)
{
    if(cpu_state->cs == USER_CODE_SEGMENT)
    {
        // todo: send SIGSEGV (refer to VINIX example)
        panic("Can't send SIGSEGV yet");
    }
    else
    {
        klog("isr", "Exception raised:");
        klog("isr", "\tException num=%d errno=%d", num, cpu_state->error_code);
        klog("isr", "\t\trip=%lx rsi=%lx", cpu_state->rip, cpu_state->rsi);
        klog("isr", "\t\trsp=%lx", cpu_state->rsp);
        panic("Caught exception in kernel: %s (aka 0x%x)", exception_names[num], num);
    }
}

void handle_interrupt(uint32_t num, __attribute__((unused)) cpu_status_t* cpu_state)
{
    lapic_eoi();
    panic("Caught interrupt %d, not handled yet", num);
}

void isr_init() {
    uint64_t* isrs = (uint64_t*)((void*)interrupt_service_routines);
    klog("isr", "ISR table at %x", isrs);
    for (uint16_t i = 0; i < 32; i++)
    {
        switch (i)
        {
            case 14:
            {
                // todo: change IST to 3 when TSS is working correctly
                set_idt_entry(i, 0x8e, KERNEL_CODE_SEGMENT, 0, (void (*)())isrs[i]);
                interrupt_table[i] = (void*)handle_pagefault;
                break;
            }
            default:
            {
                set_idt_entry(i, 0x8e, KERNEL_CODE_SEGMENT, 0, (void (*)())isrs[i]);
                interrupt_table[i] = (void*)handle_exception;
                break;
            }
        }
    }

    for (uint16_t i = 32; i < 256; i++)
    {
        set_idt_entry(i, 0x8e, KERNEL_CODE_SEGMENT, 0, (void (*)())isrs[i]);
        interrupt_table[i] = (void*)handle_interrupt;
    }

    abort_vector = idt_allocate_vector();
    interrupt_table[abort_vector] = (void*)handle_abort;
    set_ist(abort_vector, 4);
}
