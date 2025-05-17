#include <cpu/smp.h>
#include <stddef.h>
#include <klog/klog.h>
#include <stdatomic.h>
#include <malloc.h>
#include <gdt/gdt.h>
#include <interrupt/idt.h>
#include <mem/pagemap.h>
#include <mem/vmm.h>
#include <mem/pmm.h>
#include <math/si.h>
#include <cpu/msr.h>
#include <sys/syscall.h>
#include <cpu/cpu.h>
#include <scheduler/scheduler.h>
#include <interrupt/apic.h>
#include <time/pit.h>

uint32_t bsp_lapic_id = 0;
local_cpu_t** local_cpus;
uint64_t cpu_count = 0;
bool have_smp = false;

void smp_init(struct limine_smp_response* smp_response)
{
    if(smp_response == NULL)
    {
        klog("smp", "SMP Disabled: No response from limine");
        return;
    }
    klog("smp", "BSP LAPIC ID: %x", smp_response->bsp_lapic_id);
    klog("smp", "Total CPU count: %d", smp_response->cpu_count);

    struct limine_smp_info** smp_info_array = smp_response->cpus;
    bsp_lapic_id = smp_response->bsp_lapic_id;

    local_cpus = (local_cpu_t**) malloc(sizeof(local_cpu_t*) * smp_response->cpu_count);
    cpu_count = smp_response->cpu_count;

    for (uint64_t i = 0; i < smp_response->cpu_count; i++)
    {
        klog("smp", "Bringing cpu %d online", i);
        local_cpu_t* local_cpu = malloc(sizeof(local_cpu_t));
        local_cpus[i] = local_cpu;

        struct limine_smp_info* smp_info = smp_info_array[i];

        smp_info->extra_argument = (uint64_t)local_cpu;

        local_cpu->cpu_number = i;

        // don't stall the current CPU, we still need it to finish booting!
        if (smp_info->lapic_id == smp_response->bsp_lapic_id)
        {
            cpu_init(smp_info);
            continue;
        }

        smp_info->goto_address = cpu_init;

        // stall while offline
        while (atomic_load(&local_cpu->online) == 0);
    }
    have_smp = true;
    klog("smp", "All CPUs brought online");
}

void cpu_init(struct limine_smp_info* smp_info)
{
    local_cpu_t* local_cpu = (local_cpu_t*)smp_info->extra_argument;
    uint64_t cpu_number = local_cpu->cpu_number;
    local_cpu->lapic_id = smp_info->lapic_id;

    klog("smp", "Initializing CPU %d", cpu_number);

    gdt_reload();
    idt_reload();

    // ChatGPT suggested adding this to make sure the alignments are all correct,
    // however I was never in any doubt...
    _Static_assert(_Alignof(local_cpu_t) % _Alignof(task_state_segment_t) == 0, "TSS is not properly aligned");
    gdt_load_tss((void*) &local_cpu->tss);

    local_cpu->tss.ist4 = (uint64_t)&local_cpu->abort_stack[ABORT_STACK_SIZE - 1];

    switch_pagemap(&g_kernel_pagemap);

    uint64_t stack_size = (uint64_t)0x200000;
    void* common_int_stack_phys = pmm_alloc(stack_size / PAGE_SIZE);
    uint64_t* common_int_stack = (uint64_t*)((uint64_t)common_int_stack_phys + stack_size + HIGHER_HALF);
    local_cpu->tss.rsp0 = (uint64_t)common_int_stack;

    void* sched_stack_phys = pmm_alloc(stack_size / PAGE_SIZE);
    uint64_t* sched_stack = (uint64_t*)((uint64_t)sched_stack_phys + stack_size + HIGHER_HALF);
    local_cpu->tss.ist1 = (uint64_t) sched_stack;

    // TODO: a lot of magic numbers in this code, which was ported from VINIX.
    // gotta figure out what they're doing and move them to macros

    // enable syscall
    uint64_t efer = rdmsr(0xc0000080);
    efer |= 1;
    wrmsr(0xc0000080, efer);
    wrmsr(0xc0000081, 0x0033002800000000);

    // entry address
    wrmsr(0xc0000082, (uint64_t)((void*)syscall_entry));

    // ported from VINIX - this sets up the IA32_ENERGY_PERF_BIAS register, putting it in performance mode (unsetting energy-efficient mode).
    wrmsr(0x00000084, (uint64_t)~((uint32_t)0x002));

    // enable PAT (write-combining/write-protect)
    uint64_t pat_msr = rdmsr(0x277);
    pat_msr &= 0xffffffff;
    pat_msr |= ((uint64_t) 0x0105) << 32;
    wrmsr(0x277, pat_msr);

    set_gs_base((uint64_t) &local_cpu->cpu_number);
    set_kernel_gs_base((uint64_t) &local_cpu->cpu_number);

    // enable sse/sse2
    uint64_t cr0 = read_cr0();
    cr0 &= ~(1 << 2);
    cr0 |= (1 << 1);
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= (3 << 9);
    write_cr4(cr4);

    uint32_t a, b, c, d;
    bool success = cpu_id(1, 0, &a, &b, &c, &d);
    if(success && ((c & CPUID_XSAVE) != 0))
    {
        if (cpu_number == 0)
        {
            klog("fpu", "XSAVE supported");
        }

        cr4 = read_cr4();
        cr4 |= (1 << 18);
        write_cr4(cr4);

        uint64_t xcr0 = 0;
        if (cpu_number == 0)
        {
            klog("fpu", "Saving x87 state using xsave");
        }
        xcr0 |= (1 <<0);
        if (cpu_number == 0)
        {
            klog("fpu", "Saving SSE state using xsave");
        }
        xcr0 |= (1 << 1);

        if ((c & CPUID_AVX) != 0)
        {
            if (cpu_number == 0)
            {
                klog("fpu", "Saving AVX state using xsave");
            }
            xcr0 |= (1 << 2);
        }

        success = cpu_id(7, 0, &a, &b, &c, &d);
        if(success && ((b * CPUID_AVX512) != 0))
        {
            if (cpu_number == 0)
            {
                klog("fpu", "Saving AVX-512 state using xsave");
            }
            xcr0 |= (1 << 5);
            xcr0 |= (1 << 6);
            xcr0 |= (1 << 7);
        }

        wrxcr(0, xcr0);

        success = cpu_id(0xd, 0, &a, &b, &c, &d);
        if (!success)
        {
            panic("CPUID failure");
        }

        fpu_storage_size = (uint64_t) c;
        fpu_save = xsave;
        fpu_restore = xrstor;
    }
    else
    {
        if (cpu_number == 0)
        {
            klog("fpu", "Using legacy fxsave");
        }
        fpu_storage_size = (uint64_t) 512;
        fpu_save = fxsave;
        fpu_restore = fxrstor;
    }

    lapic_enable(0xff);
    asm volatile ( "sti" );

    lapic_timer_calibrate(local_cpu);

    klog("smp", "CPU %d online!", cpu_number);

    atomic_fetch_add(&local_cpu->online, 1);

    if (cpu_number != 0)
    {
        while (atomic_load(&scheduler_ready) == false);
        scheduler_await();
    }
}

local_cpu_t* cpu_get_current()
{
    bool ints = cpu_interrupt_state();
    if(ints)
    {
        klog("smp", "Attempted to get current CPU struct without disabling interrupts");
        panic("Fetching current CPU without disabling interrupts is dangerous!");
    }

    uint64_t cpu_number = 0;
    asm volatile (
        "movq %%gs:0, %0"
        : "=r" (cpu_number)
    );
    return local_cpus[cpu_number];
}
