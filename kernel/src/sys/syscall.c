#include "syscall.h"
#include <panic.h>
#include <klog/klog.h>
#include <proc/proc.h>

#define SYSCALL_NUM_ENTRIES 1

typedef void (*syscall_fn_t)();

syscall_fn_t syscall_table[SYSCALL_NUM_ENTRIES];

// syscall_handler - C handler called from syscall_entry.S
//
// This function is called from the assembly syscall entry point after:
// - swapgs has been executed (GS now points to thread_t)
// - User RSP saved, kernel stack loaded
// - RCX/R11 (return RIP/RFLAGS) saved on kernel stack
// - Syscall number stored in thread->syscall_num
//
// The return value in RAX will be passed back to userspace.
void syscall_handler()
{
    // get syscall number from current thread
    thread_t* current_thread = get_current_thread();
    if (current_thread->syscall_num >= SYSCALL_NUM_ENTRIES) {
        // todo: probably shouldn't panic?
        panic("Unrecognised syscall %d", current_thread->syscall_num);
    } else {
        // call the entry from the syscall table
        // todo: how do we pass arguments?  They should be present on the stack at this point, but uh...
        syscall_table[current_thread->syscall_num]();
    }
}

void syscall_init()
{
    syscall_table[SYS_KLOG] = (syscall_fn_t) syscall_klog;
}
