#pragma once

#include <stdbool.h>
#include <cpu/msr.h>

typedef void (*fpuSaveFn)(void*);
typedef void (*fpuRestoreFn)(void*);

#define CPUID_XSAVE (1 << 26)
#define CPUID_AVX (1 << 28)
#define CPUID_AVX512 (1 << 16)

static inline void set_kernel_gs_base(uint64_t ptr)
{
    wrmsr(0xc0000102, ptr);
}

static inline uint64_t get_kernel_gs_base()
{
    return rdmsr(0xc0000102);
}

static inline void set_gs_base(uint64_t ptr)
{
    wrmsr(0xc0000101, ptr);
}

static inline uint64_t get_gs_base()
{
    return rdmsr(0xc0000101);
}

static inline void set_fs_base(uint64_t ptr)
{
    wrmsr(0xc0000100, ptr);
}

static inline uint64_t get_fs_base()
{
    return rdmsr(0xc0000100);
}

static inline void wrxcr(uint32_t reg, uint64_t val)
{
    uint32_t a = (uint32_t)val;
    uint32_t d = (uint32_t)(val >> 32);
    asm volatile (
        "xsetbv"
        : : "a" (a),
        "d" (d),
        "c" (reg)
        : "memory"
    );
}

void xsave(void* region);
void xrstor(void* region);
void fxsave(void* region);
void fxrstor(void* region);

static inline bool cpu_id(uint32_t leaf, uint32_t subleaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
{
    uint32_t cpuid_max = 0;
    asm volatile (
        "cpuid"
        : "=a" (cpuid_max)
        : "a" (leaf & 0x80000000)
        : "%rbx", "%rcx", "%rdx"
    );
    if (leaf > cpuid_max) {
        return false;
    }
    asm volatile(
        "cpuid"
        :   "=a" (*a),
            "=b" (*b),
            "=c" (*c),
            "=d" (*d)
            :   "a" (leaf),
                "c" (subleaf)
    );
    return true;
}

static inline bool cpu_interrupt_state()
{
    uint64_t f = 0;
    asm volatile (
        "pushfq\n"
        "pop %0"
        : "=r" (f)
    );
    return (f & (1 << 9)) != 0;
}

// Save current interrupt state (RFLAGS) and disable interrupts.
// Returns the saved flags - pass to irq_restore() to restore previous state.
static inline uint64_t irq_save_and_disable(void)
{
    uint64_t flags;
    asm volatile (
        "pushfq\n\t"
        "pop %0\n\t"
        "cli"
        : "=r" (flags)
        :
        : "memory"
    );
    return flags;
}

// Restore interrupt state from previously saved flags.
// Use with irq_save_and_disable() - don't just pass arbitrary values.
static inline void irq_restore(uint64_t flags)
{
    asm volatile (
        "push %0\n\t"
        "popfq"
        :
        : "r" (flags)
        : "memory", "cc"
    );
}

static inline uint64_t cpu_rdtsc()
{
    uint32_t a = 0;
    uint32_t d = 0;
    asm volatile(
        "rdtsc"
        :   "=a" (a),
            "=d" (d)
    );
    return (uint64_t)a | ((uint64_t)d << 32);
}

static inline uint32_t cpu_rdseed32()
{
    uint32_t a = 0;
    asm volatile ("rdseed %%eax" : "=a" (a));
    return a;
}

static inline uint32_t cpu_rdrand32()
{
    uint32_t a = 0;
    asm volatile ("rdrand %%eax" : "=a" (a));
    return a;
}

static inline uint64_t cpu_rdrand64()
{
    uint64_t a = 0;
    asm volatile ("rdrand %%rax" : "=a" (a));
    return a;
}


extern uint64_t fpu_storage_size;
// function pointers
extern fpuSaveFn fpu_save;
extern fpuRestoreFn fpu_restore;

