#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

// =============================================================================
// Lock-Free Kernel Logging System
// =============================================================================
//
// This logging system uses a lock-free MPSC (multi-producer, single-consumer)
// ring buffer to allow safe logging from any context, including ISRs.
//
// Design:
// - Producers (any code calling klog) use atomic fetch-add to reserve a slot
// - Each slot has a sequence number for coordination
// - A dedicated consumer thread drains entries to the terminal
// - If the buffer is full, new messages are dropped (never blocks)
//
// The sequence number protocol:
// - slot[i].sequence == pos means: slot is available for producer at position pos
// - slot[i].sequence == pos+1 means: slot contains data ready for consumer
// - After consumer processes pos: sequence = pos + RING_SIZE (available for reuse)
//

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

#define KLOG_RING_SIZE      1024        // Must be power of 2
#define KLOG_RING_MASK      (KLOG_RING_SIZE - 1)
#define KLOG_MODULE_LEN     16          // Max module name length (including null)
#define KLOG_MESSAGE_LEN    200         // Max message length (including null)

// -----------------------------------------------------------------------------
// Data Structures
// -----------------------------------------------------------------------------

// A single log entry (256 bytes for nice alignment)
typedef struct {
    _Atomic uint64_t sequence;          // Lock-free coordination
    uint64_t timestamp_ms;              // Milliseconds since boot
    uint8_t cpu_id;                     // Which CPU logged this
    uint8_t _reserved[7];               // Padding for alignment
    char module[KLOG_MODULE_LEN];       // Module name (e.g., "sched", "e1000")
    char message[KLOG_MESSAGE_LEN];     // The formatted message
    uint8_t _pad[16];                   // Pad to 256 bytes
} klog_entry_t;

_Static_assert(sizeof(klog_entry_t) == 256, "klog_entry_t must be 256 bytes");

// The ring buffer
typedef struct {
    _Atomic uint64_t tail;              // Next slot to reserve (producers increment)
    uint8_t _pad1[56];                  // Pad to separate cache line
    _Atomic uint64_t head;              // Next slot to consume (consumer increments)
    uint8_t _pad2[56];                  // Pad to separate cache line
    klog_entry_t entries[KLOG_RING_SIZE];
} klog_ring_t;

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

// Initialize the logging system (call early in boot)
void klog_init(void);

// Start the consumer thread (call after scheduler is ready)
void klog_start_consumer(void);

// Main logging function - safe to call from ANY context including ISRs
void klog(const char *module, const char *fmt, ...);

// Variant that takes a va_list
void vklog(const char *module, const char *fmt, va_list args);

// Flush pending log entries to terminal (for use during panic)
// This is NOT lock-free - only use when you know no other CPUs are running
void klog_flush_sync(void);

// Legacy alias - now all klog calls are "unlocked" (lock-free)
#define klog_unlocked klog

// Syscall handler for SYS_KLOG (called from syscall dispatcher)
uint64_t syscall_klog(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                      uint64_t arg3, uint64_t arg4, uint64_t arg5);

#ifdef COTTAGE_DEBUG
// Debug logging - excluded from production builds
void klog_debug(const char *module, const char *fmt, ...);
#else
#define klog_debug(...)
#endif
