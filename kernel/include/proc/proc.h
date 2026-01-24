#pragma once

#include <scheduler/event.h>
#include <stdint.h>
#include <stdatomic.h>
#include <lock/lock.h>
#include <mem/pagemap.h>
#include <stdint.h>
#include <interrupt/idt.h>
#include <lock/lock.h>

// todo: make configurable at runtime?
#define PROC_MAX_FDS 256
#define PROC_MAX_EVENTS 32
#define PROC_TIMERS 4096

// this kind of doesn't matter but if you keep it to a single byte,
// it can allow some optimisations 
#define PROC_NUM_SIGACTIONS_PER_THREAD 256

// this is technically sort of arbitrary, to a point.  Just don't go crazy.
#define PROC_MAX_PROCESSES 65536

// be careful when tuning these two as they are multiplicative of one another.
// 64 threads, 64 children = 4096+64 total threads per process
#define PROC_MAX_THREADS_PER_PROCESS 64
#define PROC_MAX_CHILD_PROCESSES 64

// some per-thread maximums
#define PROC_MAX_STACKS_PER_THREAD 64
#define PROC_MAX_SIGNAL_FDS_PER_THREAD 64

// default placements of the thread stack top and MMAP anonymous non-fixed base
// these are pretty much arbitrary, just have to make sure they don't collide
// with either the kernel (HIGHER_HALF), or any other parts of the user process'
// virtual memory space.
#define PROC_DEFAULT_THREAD_STACK_TOP 0x70000000000
#define PROC_DEFAULT_MMAP_ANON_NON_FIXED_BASE 0x80000000000

typedef struct process_s process_t;
typedef struct thread_s thread_t;

typedef struct {
    void* sa_sigaction;
    uint64_t sa_mask;
    uint64_t flags;
} sig_action_t;

typedef struct thread_s {
    _Atomic uint64_t cpuid; // which cpu is the thread running on?
    thread_t* self;
    // POSIX errno (call it errnum because errno is a macro that confuses clang)
    uint64_t errnum;
    uint64_t kernel_stack;
    uint64_t user_stack;
    // used for implementing syscalls as interrupts
    uint64_t syscall_num;

    // thread id
    uint64_t tid;
    // is the thread currently queued for running?
    // Atomic because multiple CPUs may read/write this concurrently
    // during enqueue/dequeue operations
    _Atomic bool is_in_queue;
    lock_t lock;
    process_t* process;
    cpu_status_t cpu_state;
    uint64_t gs_base;
    uint64_t fs_base;
    uint64_t pf_stack;
    uint64_t cr3;
    void* fpu_storage;
    lock_t yield_await;
    uint64_t timeslice;
    uint64_t which_event;
    void* exit_value;
    event_t exited;
    uint64_t sigentry;
    sig_action_t sigactions[PROC_NUM_SIGACTIONS_PER_THREAD];
    uint64_t pending_signals;
    uint64_t masked_signals;
    _Atomic bool enqueued_by_signal;
    void* stacks[PROC_MAX_STACKS_PER_THREAD];
    lock_t signalfds_lock;
    void* signalfds[PROC_MAX_SIGNAL_FDS_PER_THREAD];
    event_t attached_events[PROC_MAX_EVENTS];
    uint64_t attached_events_index;
} thread_t;

typedef struct process_s {
    uint64_t pid;
    uint64_t parent_pid;
    pagemap_t* pagemap;

    // Thread management - protected by threads_lock
    // Must hold this lock when:
    // - Reading/writing thread_count
    // - Reading/writing threads[] array
    // - Reading/writing thread_stack_top
    lock_t threads_lock;
    uint64_t thread_stack_top GUARDED_BY(threads_lock);
    thread_t* threads[PROC_MAX_THREADS_PER_PROCESS] GUARDED_BY(threads_lock);
    size_t thread_count GUARDED_BY(threads_lock);

    lock_t fds_lock;
    void* fds[PROC_MAX_FDS];
    struct process_t* children[PROC_MAX_CHILD_PROCESSES];
    // anonymous mmap calls will place memory at this location
    uint64_t mmap_anon_non_fixed_base;
    // todo: change to vfs_node_t* maybe?
    void* cwd;
    event_t event;
    uint64_t status;
    uint8_t itimers[PROC_TIMERS];
    char* name;
} process_t;

// global variables
extern _Atomic(process_t*) processes[PROC_MAX_PROCESSES];

thread_t* get_current_thread();
uint64_t proc_allocate_pid(process_t* process);
