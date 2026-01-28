#include <scheduler/semaphore.h>
#include <scheduler/scheduler.h>
#include <proc/proc.h>
#include <stddef.h>
#include <lock/lock.h>
#include <klog/klog.h>

void sem_init(semaphore_t* sem, int64_t initial_count) {
    // Initialize the lock
    sem->lock = (lock_t)LOCK_INITIALIZER("semaphore");
    // Set the initial count
    sem->count = initial_count;
    // Initialize wait queue (head = tail = 0)
    sem->wait_head = sem->wait_tail = 0;
}

void sem_wait(semaphore_t* sem) {
    // Acquire the semaphore's lock
    uint64_t flags;
    lock_acquire_irqsave(&sem->lock, flags);

    // TODO: Check if count > 0
    //   - If yes: decrement count, release lock, return
    //   - If no: need to block...
    if (sem->count > 0)
    {
        sem->count--;
        lock_release_irqrestore(&sem->lock, flags);
        return;
    }
    else
    {
        // treat tail+1==head as "queue is full", for simplicity's sake (one slot is wasted, this is fine)
        if (((sem->wait_tail + 1) % SEM_MAX_WAITERS) == sem->wait_head)
        {
            panic("semaphore queue full");
        }
        // To block:
        //   1. Add current thread to the wait queue
        //   2. Remove current thread from scheduler's run queue
        //   3. Release the semaphore lock (AFTER removing from run queue!)
        //   4. Yield to let another thread run
        //   5. When we wake up, we've been granted the semaphore - just return
        thread_t* current_thread = get_current_thread();
        klog("sched", "get_current_thread returned %p", current_thread);
        sem->waiters[sem->wait_tail] = current_thread;
        sem->wait_tail = (sem->wait_tail + 1) % SEM_MAX_WAITERS;
        scheduler_dequeue_thread(current_thread);
        lock_release(&sem->lock);
        scheduler_yield(true);
        irq_restore(flags);
        klog("sem", "Thread %p woke up from sem_wait", current_thread);
        return;
    }
}

bool sem_trywait(semaphore_t* sem) {
    // Acquire lock
    uint64_t flags;
    lock_acquire_irqsave(&sem->lock, flags);
    // If count > 0, decrement and return true
    if(sem->count > 0)
    {
        sem->count--;
        lock_release_irqrestore(&sem->lock, flags);
        return true;
    }
    // Otherwise return false (don't block)
    // Release lock
    lock_release_irqrestore(&sem->lock, flags);
    return false;
}

void sem_signal(semaphore_t* sem) {
    // Acquire lock
    uint64_t flags;
    lock_acquire_irqsave(&sem->lock, flags);
    // Check if any threads are waiting (head != tail)
    //   - If yes: pop one from wait queue, enqueue it to scheduler
    //   - If no: just increment count
    if(sem->wait_head != sem->wait_tail)
    {
        thread_t* thread = sem->waiters[sem->wait_head];
        sem->wait_head = (sem->wait_head + 1) % SEM_MAX_WAITERS;
        klog("sem", "Waking waiter %p", thread);
        enqueue_thread(thread, false); // TODO: enqueued_by_signal=false?
    }
    else
    {
        sem->count++;
    }

    // Release lock
    lock_release_irqrestore(&sem->lock, flags);
    // NOTE: This may be called from interrupt context!
    // Do not do anything that could block.
}
