#pragma once

// the very first syscall, a simple "klog" wrapper
#define SYS_KLOG 0

// Assembly entry point for syscall instruction (defined in syscall_entry.S)
extern void syscall_entry_asm();

// C handler called from assembly
void syscall_handler();

void syscall_init();
