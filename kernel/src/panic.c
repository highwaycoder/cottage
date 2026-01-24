#include "panic.h"
#include <lock/lock.h>
#include <term/term.h>
#include <string.h>
#include <stdbool.h>
#include <serial/serial.h>
#include <stdarg.h>
#include <stdio.h>
#include <cpu/smp.h>
#include <interrupt/apic.h>
#include <interrupt/isr.h>

// Halt and catch fire function.
__attribute__((noreturn)) static void hcf(void)
{
    asm volatile("cli");
    for (;;)
    {
        asm volatile("hlt");
    }
}

extern bool have_malloc;
extern bool have_term;
extern bool have_smp;

lock_t panic_lock = LOCK_INITIALIZER("panic_lock");

void dump_task_state_segment(const task_state_segment_t *tss) {
    term_printf("  TSS Dump:\n");
    term_printf("    RSP0: %x RSP1: %x RSP2: %x\n", tss->rsp0, tss->rsp1, tss->rsp2);
    term_printf("    IST1: %x IST2: %x IST3: %x\n", tss->ist1, tss->ist2, tss->ist3);
    term_printf("    IST4: %x IST5: %x IST6: %x IST7: %x\n", tss->ist4, tss->ist5, tss->ist6, tss->ist7);
    term_printf("    IOPB: %x\n", tss->iopb);
}

void dump_local_cpu(const local_cpu_t *cpu) {
    term_printf("Local CPU Dump:\n");
    term_printf("  CPU Number: %x Zero (current thread?): %x\n", cpu->cpu_number, cpu->zero);
    dump_task_state_segment(&(cpu->tss));
    term_printf("  LAPIC ID: %x LAPIC Timer Freq: %x\n", cpu->lapic_id, cpu->lapic_timer_freq);
    term_printf("  Online: %x Is Idle: %d\n", cpu->online, cpu->is_idle);
    term_printf("  Last Run Queue Index: %ld\n", cpu->last_run_queue_index);
    // don't put abort stacks in every build
    // this may be removed later when panics are less frequent and the extra
    // information is more useful and less of a distraction
    #ifdef COTTAGE_DBG_INCLUDE_ABORT_STACK
    term_printf("  Abort Stack:\n");
    for (int i = 0; i < ABORT_STACK_SIZE; ++i) {
        term_printf("    %x\n", cpu->abort_stack[i]);
    }
    term_printf("  Aborted: %d\n", cpu->aborted);
    #endif
}

__attribute__((noreturn)) void panic(const char *msg, ...)
{
    // only allow panicking once by locking and then never unlocking
    lock_acquire(&panic_lock);
    // don't allow interrupts after a kernel panic
    asm volatile("cli" ::: "memory");
    local_cpu_t* cpu = NULL;
    if(have_smp)
        cpu = cpu_get_current();
    va_list args, args2;
    va_copy(args2, args);

    // if SMP is ready, we should abort all _other_ CPUs
    if(have_smp)
    {
        for (size_t i = 0; i < cpu_count; i++)
        {
            if (cpu->lapic_id == local_cpus[i]->lapic_id) continue;
            lapic_send_ipi(local_cpus[i]->lapic_id, abort_vector);
            while(!atomic_load(&local_cpus[i]->aborted));
        }
    }

    if (have_term)
    {
        // we add two newlines before PANIC! to make sure we have separation from
        // the last written log line
        term_write("\n\nPANIC!\n", 7);
        va_start(args, msg);
        term_vprintf(msg, args);
        va_end(args);
        if(have_smp)
            dump_local_cpu(cpu);
    }

    hcf();
}
