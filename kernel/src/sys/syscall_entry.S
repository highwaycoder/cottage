# syscall_entry.S - Assembly entry point for the syscall instruction
#
# When the CPU executes 'syscall':
#   - RCX = user RIP (return address)
#   - R11 = user RFLAGS
#   - RIP = LSTAR MSR value (this function)
#   - RSP is unchanged (still user stack!)
#   - GS base is still user's GS base
#
# To return with 'sysretq':
#   - RIP is loaded from RCX
#   - RFLAGS is loaded from R11 (masked by FMASK)
#   - CS = STAR[63:48] + 16, SS = STAR[63:48] + 8
#
# Thread structure offsets (from thread_t in proc.h):
#   gs:[0]  = cpuid
#   gs:[8]  = self (thread pointer)
#   gs:[16] = errnum
#   gs:[24] = kernel_stack
#   gs:[32] = user_stack
#   gs:[40] = syscall_num

.section .text
.align 16
.global syscall_entry_asm
.type syscall_entry_asm, @function

syscall_entry_asm:
    # Step 1: Swap GS to get kernel GS base (which points to thread_t)
    # After this, gs:[N] accesses thread structure fields
    swapgs

    # Step 2: Save user RSP to thread->user_stack (offset 32)
    # We need RSP for sysret, and we're about to clobber it
    movq %rsp, %gs:32

    # Step 3: Load kernel stack from thread->kernel_stack (offset 24)
    movq %gs:24, %rsp

    # Step 4: Save syscall number from RAX to thread->syscall_num (offset 40)
    # The syscall convention puts syscall number in RAX
    movq %rax, %gs:40

    # Step 5: Push RCX (user RIP) and R11 (user RFLAGS) onto kernel stack
    # These must be preserved for sysretq
    pushq %rcx              # user RIP
    pushq %r11              # user RFLAGS

    # Step 6: Save caller-saved registers that C might clobber
    # RDI, RSI, RDX, R8, R9 are syscall arguments (already in correct positions)
    # R10 holds the 4th argument (because syscall clobbers RCX which normally holds arg3)
    # We need to move R10 to RCX for the C calling convention
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %rax              # syscall number (also saved in thread, but we might want return value here)

    # Step 7: Set up arg3 for C calling convention
    # syscall uses R10 for 4th arg because syscall clobbers RCX
    # C calling convention uses RCX for 4th arg
    movq %r10, %rcx

    # Step 8: Enable interrupts
    # The syscall instruction clears IF in RFLAGS based on FMASK MSR
    # We want interrupts enabled during syscall processing
    sti

    # Step 9: Call the C syscall handler
    # syscall_handler(rdi, rsi, rdx, rcx, r8, r9) will dispatch to actual syscall
    call syscall_handler

    # Step 9: Disable interrupts for the return sequence
    cli

    # Step 10: Restore saved registers
    # RAX now contains syscall return value - don't restore old RAX
    addq $8, %rsp           # skip saved RAX, keep return value in RAX
    popq %r10
    popq %r9
    popq %r8
    popq %rdx
    popq %rsi
    popq %rdi

    # Step 11: Restore RCX (user RIP) and R11 (user RFLAGS)
    popq %r11               # user RFLAGS
    popq %rcx               # user RIP

    # Step 12: Restore user RSP from thread->user_stack (offset 32)
    movq %gs:32, %rsp

    # Step 13: Swap GS back to user GS base
    swapgs

    # Step 14: Return to userspace
    # sysretq loads RIP from RCX, RFLAGS from R11, sets CS/SS from STAR MSR
    sysretq

.size syscall_entry_asm, . - syscall_entry_asm
