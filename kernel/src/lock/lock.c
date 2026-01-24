/**
 * Spinlock Implementation with Debug Instrumentation
 * ===================================================
 *
 * This implementation provides basic spinlocks with optional debug features:
 *
 * In DEBUG mode (COTTAGE_DEBUG defined):
 * - Tracks which CPU owns each lock
 * - Detects recursive locking attempts (deadlock)
 * - Detects releasing locks not held or held by different CPU
 * - Records acquisition location for debugging
 *
 * IMPORTANT: Spinlocks should only be held for very short periods.
 * Holding a spinlock while sleeping or doing I/O is a bug.
 */

#include <lock/lock.h>
#include <stdatomic.h>
#include <klog/klog.h>
#include <panic.h>

// Forward declaration - we can't include smp.h here due to circular deps
// but we need cpu_get_current() for debug instrumentation
#ifdef COTTAGE_DEBUG
#include <cpu/smp.h>

// Get current CPU number safely (returns -1 if interrupts enabled or too early)
static uint64_t get_current_cpu_safe(void)
{
    // Check if interrupts are enabled - if so, we can't safely read GS
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags));
    if (flags & 0x200) {
        // Interrupts enabled - can't safely determine CPU
        return (uint64_t)-1;
    }

    // Read CPU number from GS:0
    uint64_t cpu_number;
    asm volatile("movq %%gs:0, %0" : "=r"(cpu_number));
    return cpu_number;
}
#endif

/**
 * lock_acquire - Acquire a spinlock, blocking until available
 *
 * This function spins until the lock becomes available. In debug mode,
 * it performs additional checks:
 *
 * 1. RECURSIVE LOCK DETECTION: If the current CPU already holds this lock,
 *    we'd spin forever (deadlock). Debug mode detects and panics.
 *
 * 2. DEADLOCK TIMEOUT: If we spin for too long (50M iterations), something
 *    is wrong. We panic with diagnostic info about who holds the lock.
 *
 * 3. OWNERSHIP TRACKING: Records which CPU acquired the lock for later
 *    debugging and for release-time validation.
 */
void lock_acquire(lock_t* lock)
{
    uint64_t caller = (uint64_t)__builtin_return_address(0);

#ifdef COTTAGE_DEBUG
    uint64_t my_cpu = get_current_cpu_safe();

    // Check for recursive locking (same CPU trying to acquire twice)
    // This would be an instant deadlock since we'd spin waiting for ourselves
    if (lock->owner_cpu == my_cpu && my_cpu != (uint64_t)-1) {
        klog("lock", "RECURSIVE LOCK DETECTED!");
        klog("lock", "  Lock address: %p", (void*)lock);
        if (lock->name) {
            klog("lock", "  Lock name: %s", lock->name);
        }
        klog("lock", "  CPU %lu already holds this lock", my_cpu);
        klog("lock", "  Original acquire: %p", (void*)lock->acquire_caller);
        klog("lock", "  Current acquire attempt: %p", (void*)caller);
        panic("Recursive lock acquisition (deadlock)");
    }
#endif

    // Spin until we acquire the lock
    // The magic number 50M gives us a long time to wait, but not forever.
    // If we hit this limit, something is seriously wrong (deadlock, bug, etc.)
    for (uint64_t i = 0; i < 50000000; i++)
    {
        if (lock_test_and_acquire(lock))
        {
            // Successfully acquired - record debug info
            lock->acquire_caller = caller;
#ifdef COTTAGE_DEBUG
            lock->owner_cpu = my_cpu;
            lock->acquire_count++;
#endif
            return;
        }

        // Pause instruction: hints to CPU we're in a spin-wait loop
        // This improves performance on hyperthreaded CPUs by yielding
        // resources to the sibling thread
        asm volatile("pause" ::: "memory");
    }

    // If we get here, we've spun for way too long - likely deadlock
    klog("lock", "DEADLOCK DETECTED - lock acquisition timeout!");
    klog("lock", "  Lock address: %p", (void*)lock);
#ifdef COTTAGE_DEBUG
    if (lock->name) {
        klog("lock", "  Lock name: %s", lock->name);
    }
    klog("lock", "  Held by CPU: %lu", lock->owner_cpu);
#endif
    klog("lock", "  Last acquired from: %p", (void*)lock->acquire_caller);
    klog("lock", "  Current acquire attempt from: %p", (void*)caller);

    panic("Deadlock detected - lock acquisition timeout");
}

/**
 * lock_release - Release a spinlock
 *
 * In debug mode, validates that:
 * 1. The lock is actually held
 * 2. The lock is held by the current CPU (not someone else's lock)
 *
 * THREAD SAFETY: This is the lock primitive implementation itself.
 * The RELEASE annotation in the header tells callers what to expect,
 * but the implementation doesn't need analysis.
 */
NO_THREAD_SAFETY_ANALYSIS
void lock_release(lock_t* lock)
{
#ifdef COTTAGE_DEBUG
    uint64_t my_cpu = get_current_cpu_safe();

    // Check if lock is actually held
    if (!atomic_load(&lock->is_locked)) {
        klog("lock", "RELEASING UNHELD LOCK!");
        klog("lock", "  Lock address: %p", (void*)lock);
        if (lock->name) {
            klog("lock", "  Lock name: %s", lock->name);
        }
        klog("lock", "  Release caller: %p", (void*)__builtin_return_address(0));
        panic("Attempted to release lock that isn't held");
    }

    // Check if we own this lock (only if we can determine our CPU)
    if (my_cpu != (uint64_t)-1 && lock->owner_cpu != my_cpu) {
        klog("lock", "RELEASING LOCK HELD BY DIFFERENT CPU!");
        klog("lock", "  Lock address: %p", (void*)lock);
        if (lock->name) {
            klog("lock", "  Lock name: %s", lock->name);
        }
        klog("lock", "  Lock owned by CPU: %lu", lock->owner_cpu);
        klog("lock", "  Release attempted by CPU: %lu", my_cpu);
        klog("lock", "  Original acquire: %p", (void*)lock->acquire_caller);
        klog("lock", "  Release caller: %p", (void*)__builtin_return_address(0));
        panic("Attempted to release lock held by different CPU");
    }

    // Clear ownership before releasing
    lock->owner_cpu = (uint64_t)-1;
#endif

    // Memory barrier then release
    // The atomic_store provides release semantics, ensuring all writes
    // before this point are visible to other CPUs before they see the
    // lock as available
    atomic_store(&lock->is_locked, false);
}

/**
 * lock_test_and_acquire - Try to acquire lock without blocking
 *
 * Returns true if the lock was successfully acquired, false if it was
 * already held by someone else.
 *
 * NOTE: The previous implementation had a bug - it returned lock->is_locked
 * instead of the CAS result. This meant it could return true even when
 * the CAS failed (if someone else held the lock).
 */
bool lock_test_and_acquire(lock_t* lock)
{
    bool expected = false;
    bool desired = true;

    // atomic_compare_exchange_strong:
    // - If lock->is_locked == expected (false), set it to desired (true) and return true
    // - If lock->is_locked != expected, leave it alone and return false
    //
    // This is the fundamental atomic primitive for implementing spinlocks.
    // It's guaranteed to be atomic across all CPUs.
    bool acquired = atomic_compare_exchange_strong(&lock->is_locked, &expected, desired);

    if (acquired) {
        // Record caller for debugging (full info recorded by lock_acquire)
        lock->acquire_caller = (uint64_t)__builtin_return_address(0);
#ifdef COTTAGE_DEBUG
        lock->owner_cpu = get_current_cpu_safe();
#endif
    }

    return acquired;
}

/**
 * lock_is_held - Check if lock is currently held
 *
 * WARNING: This is inherently racy! The lock state could change between
 * when you check and when you act on the result. Only use for debugging
 * or in situations where you have other synchronization guarantees.
 */
bool lock_is_held(lock_t* lock)
{
    return atomic_load(&lock->is_locked);
}

/**
 * lock_is_held_by_me - Check if current CPU holds this lock
 *
 * Must be called with interrupts disabled.
 */
bool lock_is_held_by_me(lock_t* lock)
{
#ifdef COTTAGE_DEBUG
    if (!atomic_load(&lock->is_locked)) {
        return false;
    }

    uint64_t my_cpu = get_current_cpu_safe();
    if (my_cpu == (uint64_t)-1) {
        // Can't determine CPU, conservatively return false
        return false;
    }

    return lock->owner_cpu == my_cpu;
#else
    // Without debug info, we can only check if lock is held at all
    return atomic_load(&lock->is_locked);
#endif
}

/**
 * lock_assert_held - Assert that lock is held (debug only)
 *
 * This function serves two purposes:
 * 1. Runtime check (debug builds): Panics if lock isn't held
 * 2. Static analysis hint: Tells Clang thread safety analyzer
 *    that we hold this lock, enabling it to check subsequent code
 */
void lock_assert_held(lock_t* lock)
{
#ifdef COTTAGE_DEBUG
    if (!lock_is_held_by_me(lock)) {
        klog("lock", "LOCK ASSERTION FAILED!");
        klog("lock", "  Lock address: %p", (void*)lock);
        if (lock->name) {
            klog("lock", "  Lock name: %s", lock->name);
        }
        klog("lock", "  Expected to be held by current CPU");
        klog("lock", "  Assertion caller: %p", (void*)__builtin_return_address(0));
        panic("Lock assertion failed - lock not held");
    }
#else
    // In release builds, at least check lock is held by someone
    if (!atomic_load(&lock->is_locked)) {
        panic("Lock assertion failed - lock not held");
    }
#endif
}
