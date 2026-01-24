#include <interrupt/apic.h>
#include <debug/debug.h>
#include <klog/klog.h>
#include <cpu/msr.h>
#include <mem/pmm.h>
#include <cpu/kio.h>
#include <stdint.h>
#include <cpu/cpu.h>
#include <time/pit.h>
#include <time/timer.h>
#include <cpu/smp.h>
#include <acpi/madt.h>

#define LAPIC_REG_ICR0 0x300
#define LAPIC_REG_ICR1 0x310
#define LAPIC_REG_SPURIOUS 0x0f0
#define LAPIC_REG_EOI 0x0b0
#define LAPIC_REG_TIMER 0x320
#define LAPIC_REG_TIMER_INITCNT 0x380
#define LAPIC_REG_TIMER_CURCNT 0x390
#define LAPIC_REG_TIMER_DIV 0x3e0

static uint64_t lapic_base = 0;

uint32_t lapic_read(uint32_t reg)
{
   if (lapic_base == 0)
   {
        lapic_base = (rdmsr(0x1b) & 0xfffff000) + HIGHER_HALF;
   } 
   return mmin((void*)(lapic_base + reg));
}

void lapic_write (uint32_t reg, uint32_t val)
{
   if (lapic_base == 0)
   {
        lapic_base = (rdmsr(0x1b) & 0xfffff000) + HIGHER_HALF;
   } 
   return mmout((void*)(lapic_base + reg), val);
}

void lapic_eoi()
{
    lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_enable(uint16_t spurious_vector)
{
    lapic_write(LAPIC_REG_SPURIOUS, lapic_read(LAPIC_REG_SPURIOUS) | (1 << 8) | spurious_vector);
}

void lapic_timer_stop()
{
    lapic_write(LAPIC_REG_TIMER_INITCNT, 0);
    lapic_write(LAPIC_REG_TIMER, (1 << 16));
}

void lapic_send_ipi(uint8_t lapic_id, uint8_t vector)
{
    lapic_write(LAPIC_REG_ICR1, (uint32_t)lapic_id << 24);
    lapic_write(LAPIC_REG_ICR0, vector);
}

void lapic_timer_calibrate(local_cpu_t* local_cpu)
{
    klog("lapic", "Calibrating lapic timer for CPU %d", local_cpu->cpu_number);
    lapic_timer_stop();
    uint64_t samples = 0xfffff;
    lapic_write(LAPIC_REG_TIMER, (1 << 16) | 0xff); // vector 0xff, masked
    lapic_write(LAPIC_REG_TIMER_DIV, 0b1011);  // Use same divisor as oneshot

    pit_set_reload_value(0xffff);

    uint64_t initial_pit_tick = pit_get_current_count();

    klog("lapic", "Waiting for %u timer ticks", samples);

    lapic_write(LAPIC_REG_TIMER_INITCNT, (uint32_t)samples);

    // Spin until LAPIC timer counts down to 0
    // Note: Don't add logging here - it would slow down calibration
    // and cause the measured frequency to be wrong
    while(lapic_read(LAPIC_REG_TIMER_CURCNT) != 0)
    {
        asm volatile("pause" ::: "memory");
    }

    uint64_t final_pit_tick = (uint64_t) pit_get_current_count();

    uint64_t pit_ticks = initial_pit_tick - final_pit_tick;

    local_cpu->lapic_timer_freq = (samples / pit_ticks) * PIT_DIVIDEND;
    
    lapic_timer_stop();
}

void lapic_timer_oneshot(local_cpu_t* local_cpu, uint8_t vector, uint64_t micros)
{
    lapic_timer_stop();

    uint64_t ticks = micros * (local_cpu->lapic_timer_freq / 1000000);
    
    lapic_write(LAPIC_REG_TIMER, vector);
    lapic_write(LAPIC_REG_TIMER_DIV, 0b1011);
    lapic_write(LAPIC_REG_TIMER_INITCNT, (uint32_t)ticks);
}

uint32_t io_apic_read(uint64_t io_apic, uint32_t reg)
{
    uint64_t base = ((uint64_t)madt_io_apics[io_apic].address) + HIGHER_HALF;
    mmout((uint32_t*)base, reg);
    return mmin((uint32_t*) base + 16);
}

void io_apic_write(uint64_t io_apic, uint32_t reg, uint32_t val)
{
    klog("ioapic", "Writing %x to register %x in IO APIC %d", val, reg, io_apic);
    uint64_t base = ((uint64_t)madt_io_apics[io_apic].address) + HIGHER_HALF;
    mmout((uint32_t*) base, reg);
    mmout((uint32_t*)(base + 16), val);
}

uint32_t io_apic_gsi_count(uint64_t io_apic)
{
    uint32_t x = io_apic_read(io_apic, 1);
    klog("ioapic", "Read %x from IO APIC reg 1", x);
    return (x & 0xff0000) >> 16;
}

uint64_t io_apic_from_gsi(uint32_t gsi)
{
    klog("ioapic", "Determining IO APIC index from GSI");
    klog("ioapic", "Looking through %d IO-APICs", madt_io_apic_count);
    for (size_t i = 0; i < madt_io_apic_count; i++)
    {
        klog("ioapic", "gsi=%d madt_io_apics[%d].gsib=%d, io_apic_gsi_count(%d)=%d",
            gsi,
            i,
            madt_io_apics[i].gsib,
            i,
            io_apic_gsi_count(i)
        );
        if (madt_io_apics[i].gsib <= gsi && madt_io_apics[i].gsib + io_apic_gsi_count(i) > gsi)
        {
            return i;
        }
    }
    
    panic("Cannot determine IO APIC from GSI (missing IO APIC?)");
}

void io_apic_set_gsi_redirect(uint32_t lapic_id, uint8_t vector, uint32_t gsi, uint16_t flags, bool status)
{
    uint64_t io_apic = io_apic_from_gsi(gsi);
    uint64_t redirect = (uint64_t) vector;

    if ((flags & (1 << 1)) != 0)
    {
        redirect |= (1 << 13);
    }

    if ((flags & (1 << 3)) != 0)
    {
        redirect |= (1 << 15);
    }

    if (!status)
    {
        redirect |= (1 << 16);
    }

    redirect |= ((uint64_t)lapic_id) << 56;

    uint64_t ioredtbl = (gsi - madt_io_apics[io_apic].gsib) * 2 + 16;

    io_apic_write(io_apic, ioredtbl, (uint32_t) redirect);
    io_apic_write(io_apic, ioredtbl + 1, (uint32_t) (redirect >> 32));
}

void io_apic_set_irq_redirect(uint32_t lapic_id, uint8_t vector, uint8_t irq, bool status)
{
    for(size_t i = 0; i < madt_iso_count; i++)
    {
        if (madt_isos[i].irq_source == irq)
        {
            if (status)
            {
                klog("apic", "IRQ %d using override");
            }
            io_apic_set_gsi_redirect(lapic_id, vector, madt_isos[i].gsi, madt_isos[i].flags, status);
            return;
        }
    }
    io_apic_set_gsi_redirect(lapic_id, vector, irq, 0, status);
}
