#include <sys/syscall.h>

// Simple syscall wrapper to print a message
// The kernel's SYS_KLOG takes just a message string pointer
void klog(const char* msg)
{
    // Issue SYSCALL instruction with SYS_KLOG
    // arg0 (RDI) = msg pointer
    // RAX = syscall number
    __asm__ volatile
    (
        "syscall"
        :
        : "a"(SYS_KLOG), "D"(msg)
        : "rcx", "r11", "memory"
    );
}

int main(void)
{
    klog("hello, syscalls!");
    // hang
    for(;;);
    return 0;
}
