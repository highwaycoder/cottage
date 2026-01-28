#pragma once

#include <stdint.h>

// the very first syscall, a simple "klog" wrapper
enum {
	SYS_KLOG            = 0,
	SYS_SOCKET          = 1,
	SYS_BIND            = 2,
	SYS_SENDTO          = 3,
	SYS_RECVFROM        = 4,
	SYS_CLOSE_SOCKET    = 5,
    SYSCALL_NUM_ENTRIES,
};

// Assembly entry point for syscall instruction (defined in syscall_entry.S)
extern void syscall_entry_asm();

// C handler called from assembly
// Takes 6 syscall arguments passed via registers (AMD64 syscall convention)
uint64_t syscall_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5);

void syscall_init();
