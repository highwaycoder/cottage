#include <elf/elf.h>
#include <scheduler/scheduler.h>
#include <stdint.h>
#include <interrupt/idt.h>
#include <klog/klog.h>
#include <mem/malloc.h>
#include <mem/vmm.h>
#include <mem/pmm.h>
#include <mem/mmap.h>
#include <cpu/smp.h>
#include <cpu/cpu.h>
#include <mem/align.h>
#include <gdt/gdt.h>
#include <interrupt/apic.h>
#include <string.h>
#include <debug/debug.h>
#include <fs/fs.h>
#include <term/term.h>

// use 2MB stack, similar to Linux
#define STACK_SIZE (uint64_t)(0x200000)
#define MAX_THREADS 512

// variables
_Atomic bool scheduler_ready = false;
_Atomic uint8_t scheduler_vector;
process_t *kernel_process;
_Atomic(thread_t *) scheduler_running_queue[MAX_THREADS];
_Atomic uint64_t working_cpus = 0;

// functions
int64_t get_next_thread(int64_t orig_i);
void scheduler_isr(uint32_t num, cpu_status_t *status);

// debug function found in panic.c (forward declared here because it isn't in panic.h)
void dump_local_cpu(const local_cpu_t *cpu);

int64_t get_next_thread(int64_t orig_i)
{
    uint64_t cpu_number = cpu_get_current()->cpu_number;
    klog("sched", "Getting next thread for cpu %d", cpu_number);
    int64_t index = orig_i + 1;

    while (1)
    {
        // wrap-around
        if (index >= MAX_THREADS)
            index = 0;

        thread_t *t = scheduler_running_queue[index];
        if (t != 0)
        {
            if (atomic_load(&t->cpuid) == cpu_number || lock_test_and_acquire(&t->lock))
            {
                return index;
            }
        }

        // only allow looping through the full queue once
        if (index == orig_i)
            break;

        index++;
    }

    return -1;
}

void scheduler_init()
{
    klog("sched", "initialising scheduler");
    scheduler_vector = idt_allocate_vector();
    klog("sched", "Allocated scheduler vector 0x%x", scheduler_vector);
    interrupt_table[scheduler_vector] = (void *)((uint64_t)scheduler_isr);
    set_ist(scheduler_vector, 1);
    kernel_process = malloc(sizeof(process_t));
    kernel_process->pagemap = &g_kernel_pagemap;
    // let everyone know we're up and running
    atomic_store(&scheduler_ready, true);
}

void scheduler_isr(__attribute__((unused)) uint32_t num, __attribute__((unused)) cpu_status_t *status)
{
    lapic_timer_stop();
    local_cpu_t *cpu = cpu_get_current();
    atomic_store(&cpu->is_idle, false);
    thread_t *current_thread = get_current_thread();
    int64_t new_index = get_next_thread(cpu->last_run_queue_index); 

    klog("sched", "Getting ready to try to run %d on %d", new_index, cpu->cpu_number);

    klog("sched", "current_thread=%x, new_index=%d, new_thread=%x", current_thread, new_index, scheduler_running_queue[new_index]);

    if (current_thread != 0)
    {
        lock_release(&current_thread->yield_await);

        // the happy case, we're just running the same thread again
        if (new_index == cpu->last_run_queue_index && current_thread->is_in_queue)
        {
            lapic_eoi();
            lapic_timer_oneshot(cpu, scheduler_vector, current_thread->timeslice);
            return;
        }
        // switch context
        current_thread->cpu_state = *status;
        current_thread->gs_base = get_kernel_gs_base();
        current_thread->fs_base = get_fs_base();
        current_thread->cr3 = read_cr3();
        fpu_save(current_thread->fpu_storage);
        atomic_store(&current_thread->cpuid, -1);
        lock_release(&current_thread->lock);

        atomic_fetch_sub(&working_cpus, 1);
    }

    if (new_index == -1)
    {
        klog("sched", "new_index == -1");
        lapic_eoi();
        set_gs_base((uint64_t)&cpu->cpu_number);
        set_kernel_gs_base((uint64_t)&cpu->cpu_number);
        cpu->last_run_queue_index = 0;
        atomic_store(&cpu->is_idle, true);
        if (atomic_load(&waiting_event_count) == 0 && atomic_load(&working_cpus) == 0)
        {
            panic("Event heartbeat has flatlined :(");
        }
        scheduler_await();
    }

    atomic_fetch_add(&working_cpus, 1);

    current_thread = scheduler_running_queue[new_index];
    cpu->last_run_queue_index = new_index;

    if (current_thread->cpu_state.cs == USER_CODE_SEGMENT)
    {
        // For user threads: GS_BASE = user's value, KERNEL_GS_BASE = thread pointer
        // After swapgs on interrupt, kernel will have GS_BASE = thread pointer
        set_gs_base(current_thread->gs_base);
        set_kernel_gs_base((uint64_t)current_thread);
    }
    else
    {
        // For kernel threads: both point to thread
        set_gs_base((uint64_t)current_thread);
        set_kernel_gs_base((uint64_t)current_thread);
    }
    klog("sched", "set current thread to %p", current_thread);
    set_fs_base(current_thread->fs_base);

    cpu->tss.ist3 = current_thread->pf_stack;

    if (read_cr3() != current_thread->cr3)
        write_cr3(current_thread->cr3);

    fpu_restore(current_thread->fpu_storage);

    atomic_store(&current_thread->cpuid, cpu->cpu_number);

    lapic_eoi();
    lapic_timer_oneshot(cpu, scheduler_vector, current_thread->timeslice);

    cpu_status_t* new_cpu_state = &current_thread->cpu_state;
    if (new_cpu_state->cs == USER_CODE_SEGMENT)
    {
        // todo: dispatch a signal?
        klog("sched", "Should dispatch signal but not yet implemented");
    }

    klog("sched", "new_cpu_state = %lp", new_cpu_state);
    dump_local_cpu(cpu);

    asm volatile(
        "movq %0, %%rsp\n"
        "popq %%rax\n"
        "movl %%eax, %%ds\n"
        "popq %%rax\n"
        "movl %%eax, %%es\n"
        "popq %%rax\n"
        "popq %%rbx\n"
        "popq %%rcx\n"
        "popq %%rdx\n"
        "popq %%rsi\n"
        "popq %%rdi\n"
        "popq %%rbp\n"
        "popq %%r8\n"
        "popq %%r9\n"
        "popq %%r10\n"
        "popq %%r11\n"
        "popq %%r12\n"
        "popq %%r13\n"
        "popq %%r14\n"
        "popq %%r15\n"
        "addq $8, %%rsp\n"

        "cmpq $%c1, 8(%%rsp)\n"
        "je 1f\n"
        "swapgs\n"
        "1:\n"
        "iretq\n"
        :
        : "rm" (new_cpu_state), "i" (KERNEL_CODE_SEGMENT)
        : "memory"
        );

    while (1);
}

void scheduler_await()
{
    // disable interrupts - we can't be interrupted while using cpu_get_current
    asm volatile("cli");
    local_cpu_t *local_cpu = cpu_get_current();
    // wait for 20ms
    lapic_timer_oneshot(local_cpu, scheduler_vector, 20000);
    // enable interrupts and run a HLT loop until the interrupt fires
    asm volatile("sti" ::: "memory");
    for (;;)
    {
        asm volatile("hlt" ::: "memory");
    }
}

bool scheduler_dequeue_thread(thread_t* thread)
{
    // shortcut for duplicate calls
    if(thread->is_in_queue == false)
    {
        return true;
    }

    for(uint64_t i = 0; i < MAX_THREADS; i++)
    {
        if(atomic_compare_exchange_strong(&scheduler_running_queue[i], &thread, NULL))
        {
            thread->is_in_queue = false;
            return true;
        }
    }
    klog("sched", "Could not find thread, but thread thinks it is running (WTF?)");

    // did not find thread, but thread thinks it is running
    // should this be a panic?! kind of fucked up situation tbh
    return false;
}

void scheduler_yield(bool save_context)
{
    asm volatile ("cli" ::: "memory");
    lapic_timer_stop();
    local_cpu_t* local_cpu = cpu_get_current();
    thread_t* current_thread = get_current_thread();

    if (save_context)
    {
         // acquire the yield await lock
         // ready to later on use the lock to wait for
         // an interrupt
         lock_acquire(&current_thread->yield_await);
    }
    else
    {
        set_gs_base((uint64_t)&local_cpu->cpu_number);
        set_kernel_gs_base((uint64_t)&local_cpu->cpu_number);
    }

    lapic_send_ipi((uint8_t)local_cpu->lapic_id, scheduler_vector);

    asm volatile("sti" ::: "memory");

    if(save_context)
    {
        // wait for a scheduler interrupt to fire and save the context
        lock_acquire(&current_thread->yield_await);
        lock_release(&current_thread->yield_await);
    }
    else
    {
        // hlt loop -- another thread should take over after the next interrupt
        while(1) asm volatile ("hlt" ::: "memory");
    }
}

__attribute__((noreturn)) 
void scheduler_dequeue_and_die()
{
    asm volatile ( "cli":::"memory" );
    thread_t* t = get_current_thread();
    klog("sched", "current thread = %p", t);

    scheduler_dequeue_thread(t);

    // for(size_t i = 0; i < PROC_MAX_STACKS_PER_THREAD; i++)
    // {
    //     pmm_free(t->stacks[i], STACK_SIZE / PAGE_SIZE);
    // }

    //free(t);
    scheduler_yield(false);
    while(1);
}

bool enqueue_thread(thread_t *thread, bool by_signal)
{
    // shortcut for duplicate calls
    if (thread->is_in_queue)
        return true;

    klog("sched", "Enqueueing thread %x", thread);

    atomic_store(&thread->enqueued_by_signal, by_signal);

    for (uint64_t i = 0; i < MAX_THREADS; i++)
    {
        thread_t *expected = NULL;
        if (atomic_compare_exchange_strong(&scheduler_running_queue[i], &expected, thread))
        {
            thread->is_in_queue = true;

            // wake up any idle CPUs
            for (uint64_t i = 0; i < cpu_count; i++)
            {
                if (atomic_load(&local_cpus[i]->is_idle))
                {
                    lapic_send_ipi(local_cpus[i]->lapic_id, scheduler_vector);
                    break;
                }
            }

            // we did manage to enqueue this thread
            return true;
        }
    }

    // we could not enqueue this thread
    return false;
}

thread_t *new_kernel_thread(void *ip, void *arg, bool autoenqueue)
{
    klog("sched", "Queueing up new kernel thread");
    void *stacks[PROC_MAX_STACKS_PER_THREAD];
    size_t stack_count = 0;
    void *stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
    stacks[stack_count++] = stack_phys;
    uint64_t stack = ((uint64_t)stack_phys) + STACK_SIZE + HIGHER_HALF;


    klog("sched", "stack=%x", stack);

    cpu_status_t cpu_state = {
        .cs = KERNEL_CODE_SEGMENT,
        .ds = KERNEL_DATA_SEGMENT,
        .es = KERNEL_DATA_SEGMENT,
        .ss = KERNEL_DATA_SEGMENT,
        .rflags = 0x202,
        .rip = (uint64_t)ip,
        .rdi = (uint64_t)arg,
        .rbp = (uint64_t)0,
        .rsp = stack
    };

    thread_t *t = malloc(sizeof(thread_t));
    t->process = kernel_process;
    t->cr3 = (uint64_t)kernel_process->pagemap->top_level;
    t->cpu_state = cpu_state;
    t->timeslice = 5000;
    t->cpuid = (uint64_t)-1;
    memcpy(t->stacks, stacks, sizeof(stacks));
    t->fpu_storage = (void *)((uint64_t)pmm_alloc(div_roundup(fpu_storage_size, PAGE_SIZE)) + HIGHER_HALF);
    t->self = t;
    t->gs_base = (uint64_t)t;


    if (autoenqueue)
    {
        if (!enqueue_thread(t, false))
        {
            panic("Kernel could not enqueue a thread, did we hit MAX_THREADS for kernel threads?");
        }
    }

    return t;
}

process_t* scheduler_new_process(process_t* old_process, pagemap_t* pagemap)
{
    process_t* new_process = malloc(sizeof(process_t));
    new_process->pagemap = 0;

    new_process->pid = proc_allocate_pid(new_process);
    
    if (old_process != NULL)
    {
        new_process->parent_pid = old_process->pid;
        new_process->pagemap = mmap_fork_pagemap(old_process->pagemap);
        new_process->thread_stack_top = old_process->thread_stack_top;
        new_process->mmap_anon_non_fixed_base = old_process->mmap_anon_non_fixed_base;
        new_process->cwd = old_process->cwd;
    } else {
        new_process->parent_pid = 0;
        new_process->pagemap = pagemap;
        new_process->thread_stack_top = PROC_DEFAULT_THREAD_STACK_TOP;
        new_process->mmap_anon_non_fixed_base = PROC_DEFAULT_MMAP_ANON_NON_FIXED_BASE;
        new_process->cwd = vfs_root;
    }

    return new_process;
}

thread_t* new_user_thread(
    process_t* process,
    bool want_elf,
    void* pc,
    void* arg,
    uint64_t requested_stack,
    int argc,
    const char* argv[],
    int envc,
    const char* envp[],
    elf_info_t elf_info,
    bool autoenqueue
)
{
    uint64_t* stack = NULL;
    // virtual memory address
    uint64_t stack_vma = 0;

    void* stacks[PROC_MAX_STACKS_PER_THREAD];
    size_t stack_count = 0;

    if (requested_stack == 0)
    {
        void* stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
        stack = (void*)(uint64_t)stack_phys + STACK_SIZE + HIGHER_HALF;
        stack_vma = process->thread_stack_top;
        process->thread_stack_top -= STACK_SIZE;
        uint64_t stack_bottom_vma = process->thread_stack_top;
        process->thread_stack_top -= PAGE_SIZE;

        if(!mmap_map_range(process->pagemap, stack_bottom_vma, (uint64_t)stack_phys, STACK_SIZE,
            MMAP_PROT_READ | MMAP_PROT_WRITE, MMAP_MAP_ANON
        ))
        {
            return NULL;
        }
    } else {
        stack = (uint64_t*)(void*)requested_stack;
        stack_vma = requested_stack;
    }

    void* kernel_stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
    stacks[stack_count++] = kernel_stack_phys;
    uint64_t kernel_stack = (uint64_t)kernel_stack_phys + STACK_SIZE + HIGHER_HALF;

    void* pf_stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
    stacks[stack_count++] = pf_stack_phys;
    uint64_t pf_stack = (uint64_t)pf_stack_phys + STACK_SIZE + HIGHER_HALF;
    
    cpu_status_t cpu_status = (cpu_status_t){
        .cs = USER_CODE_SEGMENT,
        .ds = USER_DATA_SEGMENT,
        .es = USER_DATA_SEGMENT,
        .ss = USER_DATA_SEGMENT,
        .rflags = 0x202,
        .rip = (uint64_t)pc,
        .rdi = (uint64_t)arg,
        .rsp = (uint64_t)stack_vma
    };

    thread_t* t = malloc(sizeof(thread_t));
    *t = (thread_t){
        .process = process,
        .cr3 = (uint64_t)process->pagemap->top_level,
        .cpu_state = cpu_status,
        .timeslice = 5000,
        .cpuid = -1,
        .kernel_stack = kernel_stack,
        .pf_stack = pf_stack,
        .stacks = {stacks}, // <-- this seems sus to me...
        .fpu_storage = (void*)(uint64_t)(pmm_alloc(div_roundup(fpu_storage_size, PAGE_SIZE)) + HIGHER_HALF)
    };

    t->self = t;
    t->gs_base = 0;
    t->fs_base = 0;

    fpu_restore(t->fpu_storage);

    // reference: https://www.felixcloutier.com/x86/fldcw
    // this instruction loads the operand into the FPU control word,
    // which is used to put the FPU into a special mode:
    // reference for the bit layout: http://web.archive.org/web/20190506231051/https://software.intel.com/en-us/articles/x87-and-sse-floating-point-assists-in-ia-32-flush-to-zero-ftz-and-denormals-are-zero-daz/
    // we want to mask the following exceptions:
    //      - invalid operand
    //      - denormal operand
    //      - divide by zero
    //      - overflow
    //      - underflow
    //      - precision
    //  (basically, all exceptions get masked)
    // bits 6 and 7 are unused
    // bits 8 and 9 refer to the precision control,
    // and the upper bits for controlling infinity and rounding are left alone
    uint16_t cw = 0b1100111111;
    asm volatile (
        "fldcw %0"
        ::  "m" (cw) 
        : "memory"
    );

	// references: 
    // - https://www.felixcloutier.com/x86/ldmxcsr 
    // - https://help.totalview.io/previous_releases/2019/html/index.html#page/Reference_Guide/Intelx86MXSCRRegister.html
    // this instruction loads the operand into the MXCSR control/status register
    // which is used for SSE operations.  The bitmask we send in is just to
    // basically reset everything and mask all the exceptions.
    uint32_t mxcsr = 0b1111110000000;
    asm volatile (
        "ldmxcsr %0"
        :: "m" (mxcsr)
        : "memory"
    );

    fpu_save(t->fpu_storage);

    for(size_t i = 0; i < PROC_NUM_SIGACTIONS_PER_THREAD; i++)
    {
        // not sure why we use -2 as the pointer and not NULL,
        // I'm porting bits of this from VINIX and that's what they did.
        // reference: https://github.com/vlang/vinix/blob/465c4217be4110ad37fe1be400e9ea8a48ba9b1b/kernel/modules/sched/sched.v#L452C3-L452C32
        t->sigactions[i].sa_sigaction = (void*)-2;
    }

    if(want_elf)
    {
        uint64_t* stack_top = stack;
        uint64_t orig_stack_vma = stack_vma;

        for (int i = 0; i < envc; i++)
        {
            // todo: potential major bug hiding in here, strlen probably not
            //  safe and we're copying a byte from after the end of a string?
            stack = stack - (strlen(envp[i]) + 1);
            memcpy(stack, envp[i], strlen(envp[i]) + 1);
        }
        for (int i = 0; i < argc; i++)
        {
            // todo: potential major bug hiding in here, strlen probably not
            //  safe and we're copying a byte from after the end of a string?
            stack = stack - (strlen(argv[i]) + 1);
            memcpy(stack, argv[i], strlen(argv[i]) + 1);
        }

		// re-align stack pointer to 16 bytes
        if (((argc + envc + 1) & 1) != 0)
        {
            stack--;
        }


		// the last element on the stack is the aux vector as per POSIX:
        // ref: https://www.gnu.org/software/libc/manual/html_node/Auxiliary-Vector.html
        // we zero this out for the call into the entry function, it may be
        // modified at runtime by the process but that's not up to us to control
        // zero vector should be 16 bytes long 
        stack[-1] = 0;
        stack--;
        stack[-1] = 0;
        stack--;

        // set up the aux table with info from the elf object we loaded
        stack -= 2;
        stack[0] = ELF_AT_ENTRY;
        stack[1] = elf_info.entry;
        stack -= 2;
        stack[0] = ELF_AT_PHDR;
        stack[1] = elf_info.prog_headers_ptr;
        stack -= 2;
        stack[0] = ELF_AT_PHENT;
        stack[1] = elf_info.prog_header_entry_size;
        stack -= 2;
        stack[0] = ELF_AT_PHNUM;
        stack[1] = elf_info.num_headers;

		// pass through the environment table
        stack--;
        stack[0] = 0;
        stack -= envc;
        for(int i = 0; i < envc; i++)
        {
            orig_stack_vma -= strlen(envp[i]) + 1;
            stack[i] = orig_stack_vma;
        }

        // pass through the args
        stack--;
        stack[0] = 0;
        stack -= argc;
        for(int i = 0; i < argc; i++)
        {
            orig_stack_vma -= strlen(argv[i]) + 1;
            stack[i] = orig_stack_vma;
        }

        stack--;
        stack[0] = argc;

		// reduce the stack pointer by the number of elements we just pushed
        t->cpu_state.rsp -= (stack_top - stack);
    }

    if (autoenqueue)
    {
        enqueue_thread(t, false);
    }

	// todo: bounds checking!
    t->tid = process->thread_count;
    process->threads[t->tid] = t;
    process->thread_count++;

    return t;
}
