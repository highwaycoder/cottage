#pragma once

#include <resource/resource.h>
#include <scheduler/semaphore.h>
#include <net/socket.h>  // Shared public definitions (net_addr_t, AF_*, SOCK_*, PROTO_*)

enum SOCK_STATE {
    SOCK_STATE_CLOSED,
    SOCK_STATE_BOUND,
};

#define RECV_QUEUE_SIZE 32

typedef struct {
    net_addr_t remote_addr;
    size_t len;
    uint8_t payload[];
} datagram_t;

typedef struct {
    datagram_t* recv_queue[RECV_QUEUE_SIZE];
    uint32_t recv_queue_head;
    uint32_t recv_queue_tail;
    semaphore_t sem;
} udp_pcb_t;

typedef struct {
    resource_t resource;

    // meta information about the socket's class, useful for knowing how to handle datastreams
    int domain;
    int type;
    int protocol;

    // state
    int state;

    // addresses
    net_addr_t local_addr;
    net_addr_t remote_addr;

    // protocol state
    void* pcb;
} socket_resource_t;

// TODO: idk if we actually need this function? maybe for initializing the port tables?
void socket_init();

// resource_t vtable implementations for sockets
bool    socket_grow(resource_t* self, void* handle, uint64_t size);
int64_t socket_read(resource_t* self, void* handle, void* buf, uint64_t loc, uint64_t count);
int64_t socket_write(resource_t* self, void* handle, void* buf, uint64_t loc, uint64_t count);
int     socket_ioctl(resource_t* self, void* handle, uint64_t request, void* argp);
bool    socket_unref(resource_t* self, void* handle);
bool    socket_link(resource_t* self, void* handle);
bool    socket_unlink(resource_t* self, void* handle);
void*   socket_mmap(resource_t* self, uint64_t page, int flags);

// Syscall signatures match syscall_fn_t for direct table registration.
// Arguments are raw register values; each function casts internally.

// sys_socket(domain, type, protocol) -> fd or -errno
uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                    uint64_t, uint64_t, uint64_t);

// sys_bind(fd, addr_ptr) -> 0 or -errno
uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t);

// sys_sendto(fd, buf_ptr, len, dest_addr_ptr) -> bytes sent or -errno
uint64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len,
                    uint64_t dest, uint64_t, uint64_t);

// sys_recvfrom(fd, buf_ptr, len, src_addr_ptr) -> bytes received or -errno
uint64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len,
                      uint64_t src, uint64_t, uint64_t);

// sys_close_socket(fd) -> 0 or -errno
uint64_t sys_close_socket(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
