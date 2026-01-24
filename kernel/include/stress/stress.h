#ifndef STRESS_H
#define STRESS_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Scheduler Stress Testing Module
 * ================================
 *
 * This module provides various stress tests designed to expose race conditions
 * in the scheduler and thread management code. Each test targets a specific
 * type of race condition pattern.
 *
 * Race conditions require specific conditions to manifest:
 * 1. Shared mutable state (e.g., scheduler_running_queue, process->threads[])
 * 2. Concurrent access from multiple CPUs/threads
 * 3. Lack of proper synchronization
 *
 * The tests work by creating high contention on these shared structures,
 * making race windows more likely to be hit.
 */

/**
 * Configuration for stress tests.
 *
 * Tuning these values affects the probability of hitting races:
 * - Higher thread counts = more contention = more likely to hit races
 * - More iterations = more opportunities for races to manifest
 * - Shorter delays = faster context switches = tighter race windows
 */
typedef struct {
    uint32_t num_threads;       // Number of concurrent threads to spawn
    uint32_t iterations;        // How many times to repeat the test pattern
    uint32_t delay_cycles;      // CPU cycles to spin between operations (0 = no delay)
    bool verbose;               // Print detailed progress
} stress_config_t;

// Default configuration - aggressive but not system-killing
#define STRESS_CONFIG_DEFAULT { \
    .num_threads = 32,          \
    .iterations = 1000,         \
    .delay_cycles = 0,          \
    .verbose = false            \
}

// Light configuration for quick sanity checks
#define STRESS_CONFIG_LIGHT { \
    .num_threads = 8,           \
    .iterations = 100,          \
    .delay_cycles = 1000,       \
    .verbose = true             \
}

// Heavy configuration for thorough testing
#define STRESS_CONFIG_HEAVY { \
    .num_threads = 64,          \
    .iterations = 10000,        \
    .delay_cycles = 0,          \
    .verbose = false            \
}

/**
 * stress_init - Initialize the stress testing subsystem
 *
 * Must be called before running any stress tests.
 * Sets up tracking structures and counters.
 */
void stress_init(void);

/**
 * stress_run_all - Run all stress tests
 * @config: Test configuration parameters
 *
 * Runs the complete stress test suite. Each test is designed to target
 * a specific class of race condition.
 *
 * Returns: true if all tests passed, false if any test detected a race
 *          or invariant violation
 */
bool stress_run_all(stress_config_t* config);

/**
 * Individual stress tests - each targets a specific race pattern
 */

/**
 * stress_thread_create_destroy - Rapid thread lifecycle test
 *
 * This test targets races in:
 * - process->threads[] array access
 * - process->thread_count updates
 * - Thread ID allocation
 * - Scheduler queue enqueue during creation
 * - Scheduler queue dequeue during destruction
 *
 * Pattern: Many threads simultaneously create and destroy child threads.
 * This creates high contention on the thread management structures.
 *
 * Races manifest as:
 * - Two threads getting the same TID
 * - Threads disappearing from process->threads[]
 * - thread_count mismatching actual thread count
 * - Crashes due to use-after-free
 */
bool stress_thread_create_destroy(stress_config_t* config);

/**
 * stress_yield_storm - Context switch stress test
 *
 * This test targets races in:
 * - scheduler_isr thread selection (the TOCTOU you fixed!)
 * - Thread queue manipulation during context switch
 * - CPU state save/restore
 * - GS_BASE/KERNEL_GS_BASE consistency
 *
 * Pattern: Many threads continuously yield(), causing rapid context
 * switches across all CPUs.
 *
 * Races manifest as:
 * - Wrong thread resuming (state corruption)
 * - Same thread running on two CPUs simultaneously
 * - Deadlock (all CPUs waiting, no runnable threads)
 * - GS_BASE pointing to wrong structure
 */
bool stress_yield_storm(stress_config_t* config);

/**
 * stress_enqueue_dequeue - Queue manipulation stress test
 *
 * This test targets races in:
 * - enqueue_thread() finding free slots
 * - scheduler_dequeue_thread() removing threads
 * - is_in_queue flag consistency
 * - Queue becoming full spuriously
 *
 * Pattern: Threads repeatedly enqueue themselves, run briefly, then
 * dequeue and sleep, while other threads do the same.
 *
 * Races manifest as:
 * - Thread stuck thinking it's queued when it's not
 * - Thread in queue but is_in_queue=false
 * - Multiple copies of same thread in queue
 * - Queue exhaustion despite few active threads
 */
bool stress_enqueue_dequeue(stress_config_t* config);

/**
 * stress_cpu_migration - Cross-CPU thread migration test
 *
 * This test targets races in:
 * - thread->cpuid updates
 * - Per-CPU data structure access
 * - working_cpus counter
 * - IPI delivery during migration
 *
 * Pattern: Threads that yield immediately after starting, forcing
 * the scheduler to potentially pick them up on different CPUs.
 *
 * Races manifest as:
 * - thread->cpuid showing CPU A while actually on CPU B
 * - working_cpus underflow/overflow
 * - Lost IPIs causing threads to stall
 */
bool stress_cpu_migration(stress_config_t* config);

/**
 * stress_invariant_check - Continuous invariant verification
 *
 * This runs alongside other stress tests to continuously verify
 * that scheduler invariants hold:
 *
 * Invariants checked:
 * 1. Every thread in scheduler_running_queue has is_in_queue=true
 * 2. No thread appears in the queue more than once
 * 3. No thread has cpuid != -1 unless actually running
 * 4. working_cpus equals count of threads with cpuid != -1
 * 5. process->thread_count matches actual threads in process->threads[]
 *
 * This is valuable because it can detect races even when they don't
 * cause an immediate crash - the invariant violation proves a race
 * occurred even if the system keeps running.
 */
bool stress_invariant_check(void);

/**
 * stress_get_stats - Get statistics from the last test run
 *
 * Returns statistics about thread operations performed, useful for
 * understanding test coverage and detecting partial failures.
 */
typedef struct {
    uint64_t threads_created;
    uint64_t threads_destroyed;
    uint64_t context_switches;
    uint64_t enqueue_operations;
    uint64_t dequeue_operations;
    uint64_t invariant_checks;
    uint64_t invariant_failures;
} stress_stats_t;

void stress_get_stats(stress_stats_t* stats);

#endif // STRESS_H
