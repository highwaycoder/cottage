#pragma once

/**
 * Spinlock Implementation with Thread Safety Analysis
 * ====================================================
 *
 * This header provides spinlock primitives with two key features:
 *
 * 1. CLANG THREAD SAFETY ANALYSIS
 *    Compile-time checking that catches many race conditions. Clang analyzes
 *    which locks protect which data and warns if you access data without
 *    holding the required lock.
 *
 *    Example:
 *      struct foo {
 *          lock_t lock;
 *          int counter GUARDED_BY(lock);  // counter is protected by lock
 *      };
 *
 *      void increment(struct foo* f) {
 *          f->counter++;  // WARNING: accessing 'counter' requires holding 'lock'
 *      }
 *
 *      void increment_safe(struct foo* f) {
 *          lock_acquire(&f->lock);
 *          f->counter++;  // OK: lock is held
 *          lock_release(&f->lock);
 *      }
 *
 * 2. RUNTIME DEBUG INSTRUMENTATION (when COTTAGE_DEBUG is defined)
 *    - Tracks which CPU owns each lock
 *    - Detects recursive locking (deadlock)
 *    - Detects releasing a lock you don't own
 *    - Records lock acquisition location for debugging
 *
 * ANNOTATION QUICK REFERENCE:
 *   GUARDED_BY(lock)           - Field is protected by lock
 *   PT_GUARDED_BY(lock)        - Pointer target is protected by lock
 *   REQUIRES(lock)             - Function requires lock to be held on entry
 *   EXCLUDES(lock)             - Function requires lock NOT to be held
 *   ACQUIRE(lock)              - Function acquires lock (doesn't release)
 *   RELEASE(lock)              - Function releases lock
 *   NO_THREAD_SAFETY_ANALYSIS  - Disable analysis for this function
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Clang Thread Safety Annotation Macros
// ============================================================================
// These are no-ops for non-clang compilers but enable compile-time race
// detection with clang's -Wthread-safety flag.

#if defined(__clang__)
    // Capability annotations - declare that something is a lock
    #define CAPABILITY(x)               __attribute__((capability(x)))
    #define SCOPED_CAPABILITY           __attribute__((scoped_lockable))

    // Data annotations - declare what lock protects data
    #define GUARDED_BY(x)               __attribute__((guarded_by(x)))
    #define PT_GUARDED_BY(x)            __attribute__((pt_guarded_by(x)))

    // Function annotations - declare lock requirements
    #define REQUIRES(...)               __attribute__((requires_capability(__VA_ARGS__)))
    #define REQUIRES_SHARED(...)        __attribute__((requires_shared_capability(__VA_ARGS__)))
    #define EXCLUDES(...)               __attribute__((locks_excluded(__VA_ARGS__)))
    #define ACQUIRE(...)                __attribute__((acquire_capability(__VA_ARGS__)))
    #define ACQUIRE_SHARED(...)         __attribute__((acquire_shared_capability(__VA_ARGS__)))
    #define RELEASE(...)                __attribute__((release_capability(__VA_ARGS__)))
    #define RELEASE_SHARED(...)         __attribute__((release_shared_capability(__VA_ARGS__)))
    #define TRY_ACQUIRE(...)            __attribute__((try_acquire_capability(__VA_ARGS__)))
    #define TRY_ACQUIRE_SHARED(...)     __attribute__((try_acquire_shared_capability(__VA_ARGS__)))
    #define ASSERT_CAPABILITY(x)        __attribute__((assert_capability(x)))
    #define ASSERT_SHARED_CAPABILITY(x) __attribute__((assert_shared_capability(x)))

    // Escape hatch - disable analysis for complex cases
    #define NO_THREAD_SAFETY_ANALYSIS   __attribute__((no_thread_safety_analysis))

    // Return type annotation
    #define RETURN_CAPABILITY(x)        __attribute__((lock_returned(x)))
#else
    // For non-clang compilers, these are all no-ops
    #define CAPABILITY(x)
    #define SCOPED_CAPABILITY
    #define GUARDED_BY(x)
    #define PT_GUARDED_BY(x)
    #define REQUIRES(...)
    #define REQUIRES_SHARED(...)
    #define EXCLUDES(...)
    #define ACQUIRE(...)
    #define ACQUIRE_SHARED(...)
    #define RELEASE(...)
    #define RELEASE_SHARED(...)
    #define TRY_ACQUIRE(...)
    #define TRY_ACQUIRE_SHARED(...)
    #define ASSERT_CAPABILITY(x)
    #define ASSERT_SHARED_CAPABILITY(x)
    #define NO_THREAD_SAFETY_ANALYSIS
    #define RETURN_CAPABILITY(x)
#endif

// ============================================================================
// Lock Structure
// ============================================================================

/**
 * Spinlock type
 *
 * The CAPABILITY("mutex") annotation tells Clang this type represents a lock.
 * This enables all the thread safety checking for code using this lock.
 */
typedef struct CAPABILITY("mutex") {
    _Atomic bool is_locked;

    // Debug info: who acquired the lock and from where
    uint64_t owner_cpu;         // CPU number that holds the lock (-1 if unlocked)
    uint64_t acquire_caller;    // Return address of lock_acquire caller
    uint64_t acquire_count;     // How many times acquired (detect recursive)

#ifdef COTTAGE_DEBUG
    const char* name;           // Optional name for debugging
#endif
} lock_t;

// Static initializer for locks
#ifdef COTTAGE_DEBUG
    #define LOCK_INITIALIZER(lock_name) { \
        .is_locked = false, \
        .owner_cpu = (uint64_t)-1, \
        .acquire_caller = 0, \
        .acquire_count = 0, \
        .name = lock_name \
    }
#else
    #define LOCK_INITIALIZER(lock_name) { \
        .is_locked = false, \
        .owner_cpu = (uint64_t)-1, \
        .acquire_caller = 0, \
        .acquire_count = 0 \
    }
#endif

// ============================================================================
// Lock Functions
// ============================================================================

/**
 * lock_acquire - Acquire a spinlock
 * @lock: The lock to acquire
 *
 * Spins until the lock is acquired. In debug mode, detects:
 * - Recursive locking (same CPU acquiring same lock twice)
 * - Extremely long waits (potential deadlock)
 *
 * Thread safety: This function ACQUIRES the lock capability.
 */
void lock_acquire(lock_t* lock) ACQUIRE(lock);

/**
 * lock_release - Release a spinlock
 * @lock: The lock to release
 *
 * In debug mode, detects:
 * - Releasing a lock not held
 * - Releasing a lock held by different CPU
 *
 * Thread safety: This function RELEASES the lock capability.
 */
void lock_release(lock_t* lock) RELEASE(lock);

/**
 * lock_test_and_acquire - Try to acquire lock without blocking
 * @lock: The lock to try to acquire
 *
 * Returns: true if lock was acquired, false if already held
 *
 * Thread safety: Conditionally acquires - returns true if acquired.
 */
bool lock_test_and_acquire(lock_t* lock) TRY_ACQUIRE(true, lock);

/**
 * lock_is_held - Check if lock is currently held (debug helper)
 * @lock: The lock to check
 *
 * Returns: true if lock is held by any CPU
 *
 * NOTE: This is inherently racy - by the time you act on the result,
 * it may have changed. Only use for debugging/assertions.
 */
bool lock_is_held(lock_t* lock);

/**
 * lock_is_held_by_me - Check if current CPU holds this lock
 * @lock: The lock to check
 *
 * Returns: true if current CPU holds this lock
 *
 * Useful for assertions in code that should run with lock held.
 * Must be called with interrupts disabled.
 */
bool lock_is_held_by_me(lock_t* lock);

/**
 * lock_assert_held - Assert that lock is held (debug builds only)
 * @lock: The lock that should be held
 *
 * Panics if lock is not held. No-op in release builds.
 * Tells the thread safety analyzer that we hold this lock.
 */
void lock_assert_held(lock_t* lock) ASSERT_CAPABILITY(lock);

// ============================================================================
// Convenience Macros
// ============================================================================

/**
 * LOCK_GUARD - Scoped lock acquisition (GCC statement expression)
 *
 * Usage:
 *   LOCK_GUARD(&my_lock) {
 *       // lock is held here
 *       do_stuff();
 *   } // lock automatically released
 *
 * Note: This uses GCC extension. For portable code, use explicit
 * lock_acquire/lock_release pairs.
 */
#define LOCK_GUARD(lock) \
    for (lock_t* _guard_lock = (lock_acquire(lock), (lock)); \
         _guard_lock; \
         lock_release(_guard_lock), _guard_lock = NULL)

// ============================================================================
// Interrupt-Safe Lock Macros
// ============================================================================
// Use these when the lock may be acquired by both normal code and interrupt
// handlers. They disable interrupts before acquiring to prevent deadlock.
//
// Usage:
//   uint64_t flags;
//   lock_acquire_irqsave(&my_lock, flags);
//   // ... critical section ...
//   lock_release_irqrestore(&my_lock, flags);

#include <cpu/cpu.h>

#define lock_acquire_irqsave(lock, flags) \
    do { (flags) = irq_save_and_disable(); lock_acquire(lock); } while(0)

#define lock_release_irqrestore(lock, flags) \
    do { lock_release(lock); irq_restore(flags); } while(0)
