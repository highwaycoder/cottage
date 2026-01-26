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
   return mmin((uint32_t*)(lapic_base + reg));
}

void lapic_write (uint32_t reg, uint32_t val)
{
   if (lapic_base == 0)
   {
        lapic_base = (rdmsr(0x1b) & 0xfffff000) + HIGHER_HALF;
        klog("lapic", "LAPIC base = %p", (void*)lapic_base);
   }
   mmout((uint32_t*)(lapic_base + reg), val);
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
    klog("lapic", "Calibrating lapic timer for CPU %d using HPET", local_cpu->cpu_number);

    // HPET must be initialized before we can calibrate
    if (!have_hpet) {
        klog("lapic", "WARNING: HPET not available, using fallback 1GHz");
        local_cpu->lapic_timer_freq = 1000000000UL;
        return;
    }

    lapic_timer_stop();

    // Number of LAPIC ticks to count down - larger = more accurate but slower
    uint64_t lapic_samples = 0xfffff;

    // Configure LAPIC timer: masked (bit 16), vector 0xff (unused since masked)
    lapic_write(LAPIC_REG_TIMER, (1 << 16) | 0xff);
    lapic_write(LAPIC_REG_TIMER_DIV, 0b1011);  // Divide by 1

    // Read HPET counter before starting LAPIC timer
    uint64_t hpet_start = get_ticks();

    // Start LAPIC countdown
    lapic_write(LAPIC_REG_TIMER_INITCNT, (uint32_t)lapic_samples);

    // Spin until LAPIC timer counts down to 0
    while (lapic_read(LAPIC_REG_TIMER_CURCNT) != 0)
    {
        asm volatile("pause" ::: "memory");
    }

    // Read HPET counter after LAPIC finished
    uint64_t hpet_end = get_ticks();
    uint64_t hpet_elapsed = hpet_end - hpet_start;

    // Avoid divide by zero (shouldn't happen with working HPET)
    if (hpet_elapsed == 0) {
        klog("lapic", "WARNING: hpet_elapsed=0, using fallback 1GHz");
        local_cpu->lapic_timer_freq = 1000000000UL;
        lapic_timer_stop();
        return;
    }

    // Calculate LAPIC frequency:
    // lapic_samples ticks took hpet_elapsed HPET ticks
    // LAPIC freq = lapic_samples / (hpet_elapsed / hpet_freq)
    //            = (lapic_samples * hpet_freq) / hpet_elapsed
    uint64_t hpet_freq = get_ticks_per_second();
    local_cpu->lapic_timer_freq = (lapic_samples * hpet_freq) / hpet_elapsed;

    klog("lapic", "Calibration: hpet_elapsed=%lu hpet_freq=%lu lapic_freq=%lu",
         hpet_elapsed, hpet_freq, local_cpu->lapic_timer_freq);

    // Sanity check: frequency should be 1MHz - 100GHz
    // (QEMU's emulated LAPIC can run at very high virtual frequencies)
    if (local_cpu->lapic_timer_freq < 1000000UL ||
        local_cpu->lapic_timer_freq > 100000000000UL) {
        klog("lapic", "WARNING: unreasonable freq %lu, using fallback 1GHz",
             local_cpu->lapic_timer_freq);
        local_cpu->lapic_timer_freq = 1000000000UL;
    }

    lapic_timer_stop();
}

void lapic_timer_oneshot(local_cpu_t* local_cpu, uint8_t vector, uint64_t micros)
{
    lapic_timer_stop();

    uint64_t ticks = micros * (local_cpu->lapic_timer_freq / 1000000);

    lapic_write(LAPIC_REG_TIMER, vector);
    lapic_write(LAPIC_REG_TIMER_DIV, 0b1011);
    lapic_write(LAPIC_REG_TIMER_INITCNT, (uint32_t)ticks);

    // Timer is now set - debug logging removed for performance
    (void)lapic_read(LAPIC_REG_TIMER);  // Keep read to verify it works
}

uint32_t io_apic_read(uint64_t io_apic, uint32_t reg)
{
    uint64_t base = ((uint64_t)madt_io_apics[io_apic].address) + HIGHER_HALF;
    mmout((uint32_t*)base, reg);
    return mmin((uint32_t*)(base + 16));  // Data register at offset 0x10 (16 bytes)
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
