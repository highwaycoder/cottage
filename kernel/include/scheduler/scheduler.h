#pragma once

#include <proc/proc.h>
#include <elf/elf.h>

// Maximum threads in the scheduler queue (see issue #17 for scalability discussion)
#define MAX_THREADS 512

void scheduler_init();
void scheduler_await();
void scheduler_dequeue_and_die();
bool scheduler_dequeue_thread(thread_t* thread);
void scheduler_yield(bool save_context);
process_t* scheduler_new_process(process_t* old_process, pagemap_t* pagemap);
thread_t* new_user_thread(
    process_t* process,
    bool want_elf,
    void* pc,
    void* arg,
    uint64_t stack,
    int argc,
    const char* argv[],
    int envc,
    const char* envp[],
    elf_info_t elf_info,
    bool autoenqueue
);
thread_t* new_kernel_thread(void* ip, void* arg, bool autoenqueue);
bool enqueue_thread(thread_t* thread, bool by_signal);

extern _Atomic uint8_t scheduler_vector;
extern _Atomic bool scheduler_ready;
extern process_t* kernel_process;

// Scheduler internals exposed for stress testing and debugging
extern _Atomic(thread_t*) scheduler_running_queue[MAX_THREADS];
extern _Atomic uint64_t working_cpus;
