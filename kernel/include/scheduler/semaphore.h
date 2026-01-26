#ifndef _KERNEL_SCHEDULER_SEMAPHORE_H
#define _KERNEL_SCHEDULER_SEMAPHORE_H

#include <stdint.h>
#include <stdbool.h>
#include <lock/lock.h>

// Forward declaration - full definition in proc/proc.h
typedef struct thread_s thread_t;

// Maximum threads that can wait on a single semaphore
#define SEM_MAX_WAITERS 64

typedef struct {
    lock_t lock;                            // Protects this structure
    int64_t count;                          // Semaphore counter (signed to catch underflow bugs)

    // Wait queue - threads blocked on this semaphore
    thread_t* waiters[SEM_MAX_WAITERS];
    uint32_t wait_head;                     // Index of first waiter
    uint32_t wait_tail;                     // Index of next free slot
} semaphore_t;

// Initialize a semaphore with the given initial count
void sem_init(semaphore_t* sem, int64_t initial_count);

// Wait (decrement) - blocks if count is 0
void sem_wait(semaphore_t* sem);

// Try to wait without blocking - returns true if acquired, false if would block
bool sem_trywait(semaphore_t* sem);

// Signal (increment) - wakes one waiter if any are blocked
// Safe to call from interrupt context
void sem_signal(semaphore_t* sem);

#endif // _KERNEL_SCHEDULER_SEMAPHORE_H
