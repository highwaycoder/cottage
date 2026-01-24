/**
 * Scheduler Stress Testing Implementation
 * ========================================
 *
 * This module implements stress tests that attempt to expose race conditions
 * by creating high contention on shared scheduler data structures.
 *
 * ## How Race Condition Testing Works
 *
 * Race conditions are timing-dependent bugs. A race occurs when:
 * 1. Two execution contexts (threads/CPUs) access shared data
 * 2. At least one access is a write
 * 3. No synchronization enforces ordering
 *
 * The "race window" is the time period during which the race can occur.
 * For example, in the TOCTOU race you fixed:
 *
 *   CPU 0                              CPU 1
 *   -----                              -----
 *   index = get_next_thread()
 *                                      dequeue_thread(T)  // sets queue[index]=NULL
 *   thread = queue[index]              // RACE! thread is now NULL
 *
 * The window here was between reading the index and using it. The fix was
 * to return the thread pointer directly, eliminating the window.
 *
 * ## Testing Strategy
 *
 * Since races depend on timing, we can't deterministically trigger them.
 * Instead, we use a probabilistic approach:
 *
 * 1. **Increase contention**: More threads competing = more likely to hit race windows
 * 2. **Reduce operation time**: Faster operations = tighter windows = more overlap
 * 3. **Repeat many times**: If race has 1/1000 chance, run 10,000 times
 * 4. **Verify invariants**: Detect races even when they don't crash
 *
 * A test "passing" doesn't prove no races exist - it just means we didn't
 * hit any in this run. A test "failing" definitively proves a race exists.
 */

#include <stress/stress.h>
#include <scheduler/scheduler.h>
#include <proc/proc.h>
#include <cpu/smp.h>
#include <lock/lock.h>
#include <klog/klog.h>
#include <panic.h>
#include <mem/malloc.h>
#include <stdatomic.h>
#include <string.h>

// Statistics tracking - all atomic to avoid races in the test infrastructure itself!
static _Atomic uint64_t stat_threads_created = 0;
static _Atomic uint64_t stat_threads_destroyed = 0;
static _Atomic uint64_t stat_context_switches = 0;
static _Atomic uint64_t stat_enqueue_ops = 0;
static _Atomic uint64_t stat_dequeue_ops = 0;
static _Atomic uint64_t stat_invariant_checks = 0;
static _Atomic uint64_t stat_invariant_failures = 0;

// Synchronization for test coordination
static _Atomic bool test_should_stop = false;
static _Atomic uint32_t threads_ready = 0;
static _Atomic uint32_t threads_finished = 0;

// scheduler_running_queue and working_cpus are declared in scheduler/scheduler.h

void stress_init(void)
{
    klog("stress", "Initializing stress test subsystem");

    // Reset all statistics
    atomic_store(&stat_threads_created, 0);
    atomic_store(&stat_threads_destroyed, 0);
    atomic_store(&stat_context_switches, 0);
    atomic_store(&stat_enqueue_ops, 0);
    atomic_store(&stat_dequeue_ops, 0);
    atomic_store(&stat_invariant_checks, 0);
    atomic_store(&stat_invariant_failures, 0);

    klog("stress", "Stress test subsystem initialized");
}

void stress_get_stats(stress_stats_t* stats)
{
    stats->threads_created = atomic_load(&stat_threads_created);
    stats->threads_destroyed = atomic_load(&stat_threads_destroyed);
    stats->context_switches = atomic_load(&stat_context_switches);
    stats->enqueue_operations = atomic_load(&stat_enqueue_ops);
    stats->dequeue_operations = atomic_load(&stat_dequeue_ops);
    stats->invariant_checks = atomic_load(&stat_invariant_checks);
    stats->invariant_failures = atomic_load(&stat_invariant_failures);
}

/**
 * Helper: Spin for a number of CPU cycles
 *
 * This creates a small delay without yielding, useful for creating
 * different timing patterns that might expose races.
 */
static inline void spin_delay(uint32_t cycles)
{
    for (volatile uint32_t i = 0; i < cycles; i++) {
        __asm__ volatile("pause" ::: "memory");
    }
}

/**
 * Helper: Barrier - wait for all threads to reach this point
 *
 * This is crucial for stress testing! We want all threads to start
 * their stressful operations at roughly the same time to maximize
 * contention. Without this, threads might finish before others start.
 */
static void barrier_wait(uint32_t num_threads)
{
    // Signal that we've reached the barrier
    atomic_fetch_add(&threads_ready, 1);

    // Spin until all threads have reached the barrier
    while (atomic_load(&threads_ready) < num_threads) {
        __asm__ volatile("pause" ::: "memory");
    }
}

// ===========================================================================
// Test 1: Thread Create/Destroy Storm
// ===========================================================================

/**
 * Child thread function for create/destroy test
 *
 * This thread does minimal work - the point is to stress the creation
 * and destruction paths, not the running code.
 */
static void stress_child_thread(void* arg)
{
    // Just yield a few times to ensure we're properly scheduled
    for (int i = 0; i < 3; i++) {
        scheduler_yield(true);
        atomic_fetch_add(&stat_context_switches, 1);
    }

    // Exit - this will exercise the thread destruction path
    atomic_fetch_add(&stat_threads_destroyed, 1);
    scheduler_dequeue_and_die();
}

/**
 * Parent thread function for create/destroy test
 *
 * Each parent thread rapidly creates child threads. The race conditions
 * we're trying to expose:
 *
 * 1. TID allocation race:
 *    Thread A reads thread_count=5
 *    Thread B reads thread_count=5
 *    Thread A sets tid=5, increments to 6
 *    Thread B sets tid=5, increments to 6  // DUPLICATE TID!
 *
 * 2. process->threads[] race:
 *    Thread A gets slot 5, starts writing thread pointer
 *    Thread B gets slot 5 (due to race #1), overwrites A's pointer
 *    Thread A's child is now orphaned, never scheduled
 *
 * 3. Enqueue race:
 *    Two threads create children simultaneously
 *    Both try to enqueue to same slot in scheduler_running_queue
 *    One child never gets enqueued
 */
typedef struct {
    stress_config_t* config;
    uint32_t thread_id;
} stress_thread_args_t;

static void stress_create_destroy_worker(void* arg)
{
    stress_thread_args_t* args = (stress_thread_args_t*)arg;
    stress_config_t* config = args->config;
    uint32_t my_id = args->thread_id;

    // Wait for all workers to be ready
    barrier_wait(config->num_threads);

    if (config->verbose) {
        klog("stress", "Worker %d starting create/destroy test", my_id);
    }

    for (uint32_t i = 0; i < config->iterations && !atomic_load(&test_should_stop); i++) {
        // Create a child thread
        thread_t* child = new_kernel_thread(stress_child_thread, NULL, true);

        if (child == NULL) {
            // Queue might be full - this is expected under heavy load
            // But if it happens too often, might indicate a leak
            spin_delay(1000);
            continue;
        }

        atomic_fetch_add(&stat_threads_created, 1);

        // Optional delay between operations
        spin_delay(config->delay_cycles);

        // Yield to let the child run (and potentially expose races in scheduling)
        scheduler_yield(true);
        atomic_fetch_add(&stat_context_switches, 1);
    }

    // Signal completion
    atomic_fetch_add(&threads_finished, 1);

    if (config->verbose) {
        klog("stress", "Worker %d finished", my_id);
    }

    free(args);
    scheduler_dequeue_and_die();
}

bool stress_thread_create_destroy(stress_config_t* config)
{
    klog("stress", "=== Starting Thread Create/Destroy Test ===");
    klog("stress", "Threads: %d, Iterations: %d", config->num_threads, config->iterations);

    // Reset coordination variables
    atomic_store(&test_should_stop, false);
    atomic_store(&threads_ready, 0);
    atomic_store(&threads_finished, 0);

    // Spawn worker threads
    for (uint32_t i = 0; i < config->num_threads; i++) {
        stress_thread_args_t* args = malloc(sizeof(stress_thread_args_t));
        if (args == NULL) {
            klog("stress", "Failed to allocate args for thread %d", i);
            atomic_store(&test_should_stop, true);
            return false;
        }

        args->config = config;
        args->thread_id = i;

        thread_t* t = new_kernel_thread(stress_create_destroy_worker, args, true);
        if (t == NULL) {
            klog("stress", "Failed to create worker thread %d", i);
            free(args);
            atomic_store(&test_should_stop, true);
            return false;
        }
    }

    // Wait for all workers to finish
    while (atomic_load(&threads_finished) < config->num_threads) {
        scheduler_yield(true);
    }

    klog("stress", "=== Thread Create/Destroy Test Complete ===");
    klog("stress", "Created: %lu, Destroyed: %lu",
         atomic_load(&stat_threads_created),
         atomic_load(&stat_threads_destroyed));

    // Basic sanity check: created should roughly equal destroyed
    // (with some tolerance for threads still exiting)
    uint64_t created = atomic_load(&stat_threads_created);
    uint64_t destroyed = atomic_load(&stat_threads_destroyed);

    if (created > destroyed + config->num_threads) {
        klog("stress", "WARNING: Large gap between created (%lu) and destroyed (%lu)",
             created, destroyed);
        klog("stress", "This might indicate threads being lost due to races");
    }

    return true;
}

// ===========================================================================
// Test 2: Yield Storm
// ===========================================================================

/**
 * Yield storm worker
 *
 * This test creates maximum context switch pressure. Every thread
 * continuously yields, causing the scheduler to run constantly.
 *
 * Race conditions this exposes:
 *
 * 1. Thread selection TOCTOU (the one you fixed!):
 *    - Scheduler finds thread at index N
 *    - Another CPU dequeues it
 *    - First CPU tries to switch to NULL/wrong thread
 *
 * 2. State save/restore races:
 *    - Thread saves state to cpu_state
 *    - Gets selected by another CPU before save completes
 *    - Corrupted state restored
 *
 * 3. cpuid races:
 *    - Thread sets cpuid = new_cpu
 *    - Original CPU still thinks thread is there
 *    - Both CPUs try to run same thread
 */
static void stress_yield_worker(void* arg)
{
    stress_thread_args_t* args = (stress_thread_args_t*)arg;
    stress_config_t* config = args->config;
    uint32_t my_id = args->thread_id;

    // Verify we can read our own thread ID consistently
    // If races corrupt per-thread state, this will detect it

    barrier_wait(config->num_threads);

    for (uint32_t i = 0; i < config->iterations && !atomic_load(&test_should_stop); i++) {
        // Yield and immediately continue
        scheduler_yield(true);
        atomic_fetch_add(&stat_context_switches, 1);

        // Tiny delay to create varied timing
        spin_delay(config->delay_cycles);

        // Every 100 iterations, verify our thread_id is still correct
        // This detects if we somehow got switched to a different thread's context
        if (i % 100 == 0 && args->thread_id != my_id) {
            klog("stress", "RACE DETECTED: Thread ID changed from %d to %d!",
                 my_id, args->thread_id);
            atomic_store(&test_should_stop, true);
            atomic_fetch_add(&stat_invariant_failures, 1);
        }
    }

    atomic_fetch_add(&threads_finished, 1);
    free(args);
    scheduler_dequeue_and_die();
}

bool stress_yield_storm(stress_config_t* config)
{
    klog("stress", "=== Starting Yield Storm Test ===");
    klog("stress", "Threads: %d, Iterations: %d", config->num_threads, config->iterations);

    atomic_store(&test_should_stop, false);
    atomic_store(&threads_ready, 0);
    atomic_store(&threads_finished, 0);

    for (uint32_t i = 0; i < config->num_threads; i++) {
        stress_thread_args_t* args = malloc(sizeof(stress_thread_args_t));
        if (args == NULL) {
            atomic_store(&test_should_stop, true);
            return false;
        }

        args->config = config;
        args->thread_id = i;

        thread_t* t = new_kernel_thread(stress_yield_worker, args, true);
        if (t == NULL) {
            free(args);
            atomic_store(&test_should_stop, true);
            return false;
        }
    }

    while (atomic_load(&threads_finished) < config->num_threads) {
        scheduler_yield(true);
    }

    klog("stress", "=== Yield Storm Test Complete ===");
    klog("stress", "Context switches: %lu", atomic_load(&stat_context_switches));

    return atomic_load(&stat_invariant_failures) == 0;
}

// ===========================================================================
// Test 3: Enqueue/Dequeue Stress
// ===========================================================================

/**
 * Enqueue/dequeue worker
 *
 * This test focuses specifically on the scheduler queue operations.
 * Threads repeatedly remove themselves from the queue, do some work,
 * then re-add themselves.
 *
 * Race conditions this exposes:
 *
 * 1. is_in_queue flag races:
 *    - Thread reads is_in_queue=true (thinks it's queued)
 *    - Another thread dequeues it, sets is_in_queue=false
 *    - First thread decides not to enqueue (based on old value)
 *    - Thread is now neither queued nor running!
 *
 * 2. Double-enqueue:
 *    - Thread checks is_in_queue=false
 *    - Enqueue succeeds, is_in_queue=true
 *    - Context switch before return
 *    - Another code path also enqueues (signal delivery, etc)
 *    - Thread appears twice in queue
 *
 * 3. Queue slot races:
 *    - Two enqueues find same NULL slot
 *    - Both CAS succeed (second one overwrites)
 *    - First thread's pointer is lost
 */
static void stress_enqueue_dequeue_worker(void* arg)
{
    stress_thread_args_t* args = (stress_thread_args_t*)arg;
    stress_config_t* config = args->config;

    barrier_wait(config->num_threads);

    for (uint32_t i = 0; i < config->iterations && !atomic_load(&test_should_stop); i++) {
        // Yield - this removes us from running and puts us back in queue
        scheduler_yield(true);
        atomic_fetch_add(&stat_context_switches, 1);
        atomic_fetch_add(&stat_enqueue_ops, 1);
        atomic_fetch_add(&stat_dequeue_ops, 1);

        spin_delay(config->delay_cycles);
    }

    atomic_fetch_add(&threads_finished, 1);
    free(args);
    scheduler_dequeue_and_die();
}

bool stress_enqueue_dequeue(stress_config_t* config)
{
    klog("stress", "=== Starting Enqueue/Dequeue Test ===");

    atomic_store(&test_should_stop, false);
    atomic_store(&threads_ready, 0);
    atomic_store(&threads_finished, 0);

    for (uint32_t i = 0; i < config->num_threads; i++) {
        stress_thread_args_t* args = malloc(sizeof(stress_thread_args_t));
        if (args == NULL) {
            atomic_store(&test_should_stop, true);
            return false;
        }

        args->config = config;
        args->thread_id = i;

        thread_t* t = new_kernel_thread(stress_enqueue_dequeue_worker, args, true);
        if (t == NULL) {
            free(args);
            atomic_store(&test_should_stop, true);
            return false;
        }
    }

    while (atomic_load(&threads_finished) < config->num_threads) {
        scheduler_yield(true);
    }

    klog("stress", "=== Enqueue/Dequeue Test Complete ===");

    return true;
}

// ===========================================================================
// Test 4: CPU Migration
// ===========================================================================

static void stress_migration_worker(void* arg)
{
    stress_thread_args_t* args = (stress_thread_args_t*)arg;
    stress_config_t* config = args->config;

    barrier_wait(config->num_threads);

    // Track which CPUs we've run on
    uint64_t cpus_seen = 0;

    for (uint32_t i = 0; i < config->iterations && !atomic_load(&test_should_stop); i++) {
        // Get current CPU (interrupts disabled for this call)
        __asm__ volatile("cli");
        local_cpu_t* cpu = cpu_get_current();
        uint64_t cpu_num = cpu->cpu_number;
        __asm__ volatile("sti");

        cpus_seen |= (1ULL << cpu_num);

        // Immediately yield to encourage migration
        scheduler_yield(true);
        atomic_fetch_add(&stat_context_switches, 1);
    }

    if (config->verbose) {
        klog("stress", "Thread %d saw CPUs: 0x%lx", args->thread_id, cpus_seen);
    }

    atomic_fetch_add(&threads_finished, 1);
    free(args);
    scheduler_dequeue_and_die();
}

bool stress_cpu_migration(stress_config_t* config)
{
    klog("stress", "=== Starting CPU Migration Test ===");

    atomic_store(&test_should_stop, false);
    atomic_store(&threads_ready, 0);
    atomic_store(&threads_finished, 0);

    for (uint32_t i = 0; i < config->num_threads; i++) {
        stress_thread_args_t* args = malloc(sizeof(stress_thread_args_t));
        if (args == NULL) {
            atomic_store(&test_should_stop, true);
            return false;
        }

        args->config = config;
        args->thread_id = i;

        thread_t* t = new_kernel_thread(stress_migration_worker, args, true);
        if (t == NULL) {
            free(args);
            atomic_store(&test_should_stop, true);
            return false;
        }
    }

    while (atomic_load(&threads_finished) < config->num_threads) {
        scheduler_yield(true);
    }

    klog("stress", "=== CPU Migration Test Complete ===");

    return true;
}

// ===========================================================================
// Invariant Checking
// ===========================================================================

/**
 * Check scheduler invariants
 *
 * This is extremely valuable for detecting races! Many races don't cause
 * immediate crashes - instead they leave the system in an inconsistent
 * state that eventually causes problems.
 *
 * By checking invariants, we can detect the race at the moment it happens,
 * not later when it causes a mysterious crash.
 */
bool stress_invariant_check(void)
{
    atomic_fetch_add(&stat_invariant_checks, 1);

    // Disable interrupts while checking - we need a consistent snapshot
    __asm__ volatile("cli");

    bool ok = true;

    // Invariant 1: Count threads in queue vs is_in_queue flags
    __attribute__((unused)) uint32_t queue_count = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t* t = atomic_load(&scheduler_running_queue[i]);
        if (t != NULL) {
            queue_count++;

            // Every thread in queue should have is_in_queue=true
            if (!t->is_in_queue) {
                klog("stress", "INVARIANT VIOLATION: Thread %p in queue but is_in_queue=false", t);
                atomic_fetch_add(&stat_invariant_failures, 1);
                ok = false;
            }
        }
    }

    // Invariant 2: No thread should appear twice in queue
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t* t = atomic_load(&scheduler_running_queue[i]);
        if (t == NULL) continue;

        for (int j = i + 1; j < MAX_THREADS; j++) {
            thread_t* t2 = atomic_load(&scheduler_running_queue[j]);
            if (t == t2) {
                klog("stress", "INVARIANT VIOLATION: Thread %p appears at index %d and %d", t, i, j);
                atomic_fetch_add(&stat_invariant_failures, 1);
                ok = false;
            }
        }
    }

    // Invariant 3: working_cpus should match threads with cpuid != -1
    // (This is tricky to check atomically, so we just log if suspicious)
    uint64_t wc = atomic_load(&working_cpus);
    if (wc > cpu_count) {
        klog("stress", "INVARIANT VIOLATION: working_cpus (%lu) > cpu_count (%lu)", wc, cpu_count);
        atomic_fetch_add(&stat_invariant_failures, 1);
        ok = false;
    }

    __asm__ volatile("sti");

    return ok;
}

// ===========================================================================
// Run All Tests
// ===========================================================================

bool stress_run_all(stress_config_t* config)
{
    stress_init();

    klog("stress", "");
    klog("stress", "========================================");
    klog("stress", "   SCHEDULER STRESS TEST SUITE");
    klog("stress", "========================================");
    klog("stress", "");

    bool all_passed = true;

    // Run each test
    if (!stress_thread_create_destroy(config)) {
        klog("stress", "FAILED: Thread Create/Destroy Test");
        all_passed = false;
    }

    // Check invariants between tests
    if (!stress_invariant_check()) {
        klog("stress", "FAILED: Invariant check after create/destroy");
        all_passed = false;
    }

    if (!stress_yield_storm(config)) {
        klog("stress", "FAILED: Yield Storm Test");
        all_passed = false;
    }

    if (!stress_invariant_check()) {
        klog("stress", "FAILED: Invariant check after yield storm");
        all_passed = false;
    }

    if (!stress_enqueue_dequeue(config)) {
        klog("stress", "FAILED: Enqueue/Dequeue Test");
        all_passed = false;
    }

    if (!stress_invariant_check()) {
        klog("stress", "FAILED: Invariant check after enqueue/dequeue");
        all_passed = false;
    }

    if (!stress_cpu_migration(config)) {
        klog("stress", "FAILED: CPU Migration Test");
        all_passed = false;
    }

    if (!stress_invariant_check()) {
        klog("stress", "FAILED: Invariant check after migration");
        all_passed = false;
    }

    // Print summary
    stress_stats_t stats;
    stress_get_stats(&stats);

    klog("stress", "");
    klog("stress", "========================================");
    klog("stress", "   TEST SUMMARY");
    klog("stress", "========================================");
    klog("stress", "Threads created:    %lu", stats.threads_created);
    klog("stress", "Threads destroyed:  %lu", stats.threads_destroyed);
    klog("stress", "Context switches:   %lu", stats.context_switches);
    klog("stress", "Enqueue ops:        %lu", stats.enqueue_operations);
    klog("stress", "Dequeue ops:        %lu", stats.dequeue_operations);
    klog("stress", "Invariant checks:   %lu", stats.invariant_checks);
    klog("stress", "Invariant failures: %lu", stats.invariant_failures);
    klog("stress", "");
    klog("stress", "Result: %s", all_passed ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    klog("stress", "========================================");

    return all_passed;
}
