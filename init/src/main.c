#include <sys/syscall.h>
#include <stdint.h>
#include <stddef.h>
#include <net/socket.h>

// Simple syscall wrapper to print a message
void klog(const char* msg)
{
    __asm__ volatile (
        "syscall"
        :
        : "a"(SYS_KLOG), "D"(msg)
        : "rcx", "r11", "memory"
    );
}

// socket(domain, type, protocol) -> fd or -errno
int socket(int domain, int type, int protocol)
{
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(SYS_SOCKET), "D"(domain), "S"(type), "d"(protocol)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

// bind(fd, addr) -> 0 or -errno
int bind(int fd, net_addr_t* addr)
{
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(SYS_BIND), "D"(fd), "S"(addr)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

// sendto(fd, buf, len, dest) -> bytes sent or -errno
// Note: 4th arg goes in R10 (syscall clobbers RCX)
int64_t sendto(int fd, void* buf, size_t len, net_addr_t* dest)
{
    int64_t ret;
    register net_addr_t* r10 __asm__("r10") = dest;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(SYS_SENDTO), "D"(fd), "S"(buf), "d"(len), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// recvfrom(fd, buf, len, src) -> bytes received or -errno
// Note: 4th arg goes in R10 (syscall clobbers RCX)
int64_t recvfrom(int fd, void* buf, size_t len, net_addr_t* src)
{
    int64_t ret;
    register net_addr_t* r10 __asm__("r10") = src;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(SYS_RECVFROM), "D"(fd), "S"(buf), "d"(len), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

int main(void)
{
    klog("hello, syscalls!");

    int sockfd = socket(AF_IPV4, SOCK_DGRAM, PROTO_UDP);
    net_addr_t bind_addr = {
        .ipv4 = 0x0A00020F, // 10.0.2.15
        .family = AF_IPV4,
        .port = 0x391E,
    };
    bind(sockfd, &bind_addr);
    while(1)
    {
        char buf[2048];
        net_addr_t remote_addr;
        uint64_t bytes_read = recvfrom(sockfd, buf, 2048, &remote_addr);
        sendto(sockfd, buf, bytes_read, &remote_addr);
    }


    // hang
    for(;;);
    return 0;
}
