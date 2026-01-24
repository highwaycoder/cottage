#pragma once

#include <stdarg.h>
#include <stdint.h>

// Syscall handler for SYS_KLOG (called from syscall dispatcher)
uint64_t syscall_klog(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                      uint64_t arg3, uint64_t arg4, uint64_t arg5);
void vklog(const char* module, const char* msg, va_list args);
void klog(const char *module, const char *msg, ...);

// Lock-free variant for use in lock implementation and early boot.
// WARNING: Output may interleave with other log messages.
void klog_unlocked(const char *module, const char *msg, ...);

#ifdef COTTAGE_DEBUG
// use this to add debug logs, so that they can be distinguished from actual logs
// and excluded from production builds
void klog_debug(const char* module, const char* msg, ...);
#else
#define klog_debug(...)
#endif


