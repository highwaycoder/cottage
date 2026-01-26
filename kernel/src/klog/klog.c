/**
 * Lock-Free Kernel Logging System
 * ================================
 *
 * This implementation uses a lock-free MPSC ring buffer that allows logging
 * from any context, including interrupt handlers, without risk of deadlock.
 *
 * The key insight is using sequence numbers in each slot to coordinate
 * producers and the consumer without locks:
 *
 * Producer (any CPU, any context):
 *   1. Atomically reserve a slot by incrementing tail
 *   2. Wait until slot's sequence matches our position (slot is writable)
 *   3. Write data to the slot
 *   4. Set sequence = position + 1 (mark as ready for consumer)
 *
 * Consumer (dedicated thread):
 *   1. Check if head slot's sequence == head + 1 (ready to read)
 *   2. Read and output the entry
 *   3. Set sequence = head + RING_SIZE (mark as writable again)
 *   4. Increment head
 *
 * If the buffer is full, producers drop messages rather than blocking.
 * This is essential for ISR safety.
 */

#include <klog/klog.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <term/term.h>
#include <time/timer.h>
#include <scheduler/scheduler.h>

// -----------------------------------------------------------------------------
// External Dependencies
// -----------------------------------------------------------------------------

// Flags indicating which subsystems are ready (defined elsewhere)
extern bool have_term;
extern volatile bool have_hpet;

// We need to know if SMP is initialized to safely get CPU number
extern bool have_smp;

// Forward declaration - get current CPU number from GS segment
// Returns 0 if SMP not ready yet
static uint8_t get_cpu_id_safe(void);

// -----------------------------------------------------------------------------
// The Ring Buffer (statically allocated)
// -----------------------------------------------------------------------------

static klog_ring_t klog_ring;

// Flag to track if consumer thread is running
static _Atomic bool consumer_running = false;

// Early boot buffer - before ring is initialized, we write directly to terminal
static bool ring_initialized = false;

// Statistics (for debugging the logging system itself)
static _Atomic uint64_t dropped_count = 0;

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

void klog_init(void)
{
    // Initialize all sequence numbers
    // slot[i].sequence = i means slot i is ready for producer at position i
    for (uint64_t i = 0; i < KLOG_RING_SIZE; i++) {
        atomic_store_explicit(&klog_ring.entries[i].sequence, i, memory_order_relaxed);
    }

    atomic_store_explicit(&klog_ring.tail, 0, memory_order_relaxed);
    atomic_store_explicit(&klog_ring.head, 0, memory_order_relaxed);

    // Memory barrier to ensure all initialization is visible
    atomic_thread_fence(memory_order_seq_cst);

    ring_initialized = true;
}

// -----------------------------------------------------------------------------
// Producer - The Lock-Free Part
// -----------------------------------------------------------------------------

// Internal function that does the actual lock-free produce
static bool klog_produce(const char *module, const char *message, uint64_t timestamp, uint8_t cpu)
{
    if (!ring_initialized) {
        return false;
    }

    // Step 1: Reserve a slot atomically
    uint64_t pos = atomic_fetch_add_explicit(&klog_ring.tail, 1, memory_order_relaxed);
    klog_entry_t *slot = &klog_ring.entries[pos & KLOG_RING_MASK];

    // Step 2: Wait for the slot to be writable (previous consumer released it)
    // Expected sequence value for this position
    uint64_t expected_seq = pos;

    // Spin with a limit - if buffer is full, drop the message
    // We check if sequence matches what we expect for our position
    for (int spin = 0; spin < 1000; spin++) {
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (seq == expected_seq) {
            // Slot is ready for us to write
            goto write_slot;
        }
        if (seq < expected_seq) {
            // Consumer is behind - buffer is full
            // We can't wait (might be in ISR), so drop the message
            atomic_fetch_add(&dropped_count, 1);
            return false;
        }
        // seq > expected_seq means we're behind - shouldn't happen with fetch_add
        // but spin a bit just in case of memory ordering weirdness
        asm volatile("pause" ::: "memory");
    }

    // Timeout - buffer is full, drop the message
    atomic_fetch_add(&dropped_count, 1);
    return false;

write_slot:
    // Step 3: Write data to the slot
    slot->timestamp_ms = timestamp;
    slot->cpu_id = cpu;

    // Copy module name (truncate if needed)
    size_t module_len = strlen(module);
    if (module_len >= KLOG_MODULE_LEN) {
        module_len = KLOG_MODULE_LEN - 1;
    }
    memcpy(slot->module, module, module_len);
    slot->module[module_len] = '\0';

    // Copy message (truncate if needed)
    size_t msg_len = strlen(message);
    if (msg_len >= KLOG_MESSAGE_LEN) {
        msg_len = KLOG_MESSAGE_LEN - 1;
    }
    memcpy(slot->message, message, msg_len);
    slot->message[msg_len] = '\0';

    // Step 4: Mark slot as ready for consumer
    // The release fence ensures all our writes are visible before updating sequence
    atomic_store_explicit(&slot->sequence, pos + 1, memory_order_release);

    return true;
}

// -----------------------------------------------------------------------------
// Consumer - Runs in a Dedicated Thread
// -----------------------------------------------------------------------------

// Output a single log entry to the terminal
static void klog_output_entry(klog_entry_t *entry)
{
    if (!have_term) {
        return;
    }

    // Format: [timestamp] [module] message\n
    char line[300];  // Enough for timestamp + module + message + formatting
    int len;

    uint64_t secs = entry->timestamp_ms / 1000;
    uint64_t ms = entry->timestamp_ms % 1000;

    // Manual zero-padding for milliseconds
    if (ms < 10) {
        len = snprintf(line, sizeof(line), "[%lu.00%lu] [%s] %s\n",
                       secs, ms, entry->module, entry->message);
    } else if (ms < 100) {
        len = snprintf(line, sizeof(line), "[%lu.0%lu] [%s] %s\n",
                       secs, ms, entry->module, entry->message);
    } else {
        len = snprintf(line, sizeof(line), "[%lu.%lu] [%s] %s\n",
                       secs, ms, entry->module, entry->message);
    }

    if (len > 0) {
        term_write(line, len);
    }
}

// Consume one entry if available
// Returns true if an entry was consumed, false if queue was empty
static bool klog_consume_one(void)
{
    uint64_t head = atomic_load_explicit(&klog_ring.head, memory_order_relaxed);
    klog_entry_t *slot = &klog_ring.entries[head & KLOG_RING_MASK];

    // Check if this slot is ready for consumption
    uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
    if (seq != head + 1) {
        // Slot not ready yet (either empty or producer still writing)
        return false;
    }

    // Output the entry
    klog_output_entry(slot);

    // Mark slot as available for reuse by a future producer
    // The producer at position (head + RING_SIZE) will be able to use this slot
    atomic_store_explicit(&slot->sequence, head + KLOG_RING_SIZE, memory_order_release);

    // Advance head
    atomic_store_explicit(&klog_ring.head, head + 1, memory_order_relaxed);

    return true;
}

// Consumer thread main function
static void klog_consumer_thread(void *arg)
{
    (void)arg;

    atomic_store(&consumer_running, true);

    // Drain any entries that accumulated before the consumer started
    while (klog_consume_one()) {
        // Keep consuming
    }

    // Main loop: periodically check for new entries
    // We don't use semaphores here to avoid any possibility of the logging
    // system depending on the scheduler in a way that could cause issues
    while (1) {
        // Process all available entries
        while (klog_consume_one()) {
            // Keep consuming until empty
        }

        // Sleep briefly before checking again
        // In a more sophisticated implementation, we could use a futex or
        // similar mechanism to wake up only when there's work
        // For now, just yield and let other threads run
        scheduler_yield(true);
    }
}

void klog_start_consumer(void)
{
    if (atomic_load(&consumer_running)) {
        return;  // Already running
    }

    new_kernel_thread(klog_consumer_thread, NULL, true);
}

// -----------------------------------------------------------------------------
// Synchronous Flush (for panic and shutdown)
// -----------------------------------------------------------------------------

void klog_flush_sync(void)
{
    // Drain all pending entries
    // WARNING: This is not safe to call if other CPUs might be logging
    // It's meant for use during panic when other CPUs should be halted
    while (klog_consume_one()) {
        // Keep draining
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void vklog(const char *module, const char *fmt, va_list args)
{
    // Get timestamp and CPU ID safely
    uint64_t timestamp = have_hpet ? get_time_since_boot_ms() : 0;
    uint8_t cpu = get_cpu_id_safe();

    // Format the message
    char message[KLOG_MESSAGE_LEN];
    vsnprintf(message, sizeof(message), fmt, args);

    // Try to produce to the ring buffer
    if (ring_initialized) {
        klog_produce(module, message, timestamp, cpu);

        // If consumer isn't running yet, also output directly to terminal
        // This ensures early boot messages are visible
        if (!atomic_load(&consumer_running) && have_term) {
            // Format and output directly
            char line[300];
            int len;

            uint64_t secs = timestamp / 1000;
            uint64_t ms = timestamp % 1000;

            if (!have_hpet) {
                len = snprintf(line, sizeof(line), "[early] [%s] %s\n", module, message);
            } else if (ms < 10) {
                len = snprintf(line, sizeof(line), "[%lu.00%lu] [%s] %s\n",
                               secs, ms, module, message);
            } else if (ms < 100) {
                len = snprintf(line, sizeof(line), "[%lu.0%lu] [%s] %s\n",
                               secs, ms, module, message);
            } else {
                len = snprintf(line, sizeof(line), "[%lu.%lu] [%s] %s\n",
                               secs, ms, module, message);
            }

            if (len > 0) {
                term_write(line, len);
            }
        }
    } else {
        // Ring not initialized - output directly if we have a terminal
        if (have_term) {
            char line[300];
            int len = snprintf(line, sizeof(line), "[early] [%s] %s\n", module, message);
            if (len > 0) {
                term_write(line, len);
            }
        }
    }
}

void klog(const char *module, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vklog(module, fmt, args);
    va_end(args);
}

#ifdef COTTAGE_DEBUG
void klog_debug(const char *module, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vklog(module, fmt, args);
    va_end(args);
}
#endif

// -----------------------------------------------------------------------------
// Syscall Handler
// -----------------------------------------------------------------------------

uint64_t syscall_klog(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                      uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *msg = (const char *)arg0;

    if (msg == NULL) {
        return (uint64_t)-1;
    }

    klog("user", "%s", msg);
    return 0;
}

// -----------------------------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------------------------

static uint8_t get_cpu_id_safe(void)
{
    if (!have_smp) {
        return 0;
    }

    // Read CPU number from GS:0
    // This is safe because if have_smp is true, GS is set up correctly
    uint64_t cpu;
    asm volatile("movq %%gs:0, %0" : "=r"(cpu));

    return (uint8_t)cpu;
}
