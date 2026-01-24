#include "syscall.h"
#include <panic.h>
#include <klog/klog.h>
#include <proc/proc.h>

#define SYSCALL_NUM_ENTRIES 1

typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

syscall_fn_t syscall_table[SYSCALL_NUM_ENTRIES];

// syscall_handler - C handler called from syscall_entry.S
//
// This function is called from the assembly syscall entry point after:
// - swapgs has been executed (GS now points to thread_t)
// - User RSP saved, kernel stack loaded
// - RCX/R11 (return RIP/RFLAGS) saved on kernel stack
// - Syscall number stored in thread->syscall_num
//
// Arguments are passed in the standard System V AMD64 ABI registers:
//   arg0 (RDI), arg1 (RSI), arg2 (RDX), arg3 (R10*), arg4 (R8), arg5 (R9)
// *Note: R10 is used instead of RCX because syscall clobbers RCX (user RIP)
//
// The return value in RAX will be passed back to userspace.
uint64_t syscall_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    // get syscall number from current thread
    thread_t* current_thread = get_current_thread();
    if (current_thread->syscall_num >= SYSCALL_NUM_ENTRIES) {
        klog("syscall", "Unrecognised syscall %d", current_thread->syscall_num);
        return (uint64_t)-1;  // Return error instead of panicking
    }

    // Call the syscall handler, passing all 6 possible arguments
    return syscall_table[current_thread->syscall_num](arg0, arg1, arg2, arg3, arg4, arg5);
}

void syscall_init()
{
    syscall_table[SYS_KLOG] = (syscall_fn_t) syscall_klog;
}
