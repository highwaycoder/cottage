#include <net/arp.h>
#include <net/route.h>
#include <proc/proc.h>
#include <file/file.h>
#include <fs/fs.h>
#include <errors/errno.h>
#include <socket/socket.h>
#include <mem/malloc.h>
#include <string.h>
#include <net/endian.h>
#include <net/network.h>
#include <net/ipv4.h>
#include <cpu/cpu.h>

lock_t udp_port_table_lock;
socket_resource_t* udp_port_table[65536] = {0};

uint16_t get_next_ephemeral_port()
{
    uint16_t start = EPHEMERAL_PORT_MIN + (cpu_rdrand32() % (EPHEMERAL_PORT_MAX - EPHEMERAL_PORT_MIN + 1));
    uint16_t range = EPHEMERAL_PORT_MAX - EPHEMERAL_PORT_MIN + 1;
    for(uint16_t i = 0; i < range; i++)
    {
        uint16_t port = EPHEMERAL_PORT_MIN + ((start - EPHEMERAL_PORT_MIN + i) % range);
        if (udp_port_table[port] == NULL) return port;
    }
    return 0;
}

void socket_init()
{
    // function body left deliberately empty
}

uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                    uint64_t, uint64_t, uint64_t)
{
    if(domain != AF_IPV4)
    {
        return -EAFNOSUPPORT;
    }
    if(type != SOCK_DGRAM)
    {
        return -EINVAL;
    }
    if(protocol != PROTO_UDP)
    {
        return -EINVAL;
    }

    process_t* process = get_current_thread()->process;
    lock_acquire(&process->fds_lock);
    int cur_fd = 0;
    while(process->fds[cur_fd] != NULL && cur_fd < PROC_MAX_FDS) cur_fd++;
    if (cur_fd == PROC_MAX_FDS)
    {
        lock_release(&process->fds_lock);
        return -EMFILE;
    }

    socket_resource_t* sockresource = malloc(sizeof(socket_resource_t));
    memset(sockresource, 0, sizeof(socket_resource_t));

    // function pointers
    sockresource->resource.grow   = socket_grow  ;
    sockresource->resource.read   = socket_read  ;
    sockresource->resource.write  = socket_write ;
    sockresource->resource.ioctl  = socket_ioctl ;
    sockresource->resource.unref  = socket_unref ;
    sockresource->resource.link   = socket_link  ;
    sockresource->resource.unlink = socket_unlink;
    sockresource->resource.mmap   = socket_mmap  ;

    sockresource->domain = domain;
    sockresource->type = type;
    sockresource->protocol = protocol;
    sockresource->state = SOCK_STATE_CLOSED;

    sockresource->resource.stat.mode = STAT_IFSOCK;

    udp_pcb_t* pcb = malloc(sizeof(udp_pcb_t));
    memset(pcb, 0, sizeof(udp_pcb_t));
    sem_init(&pcb->sem, 0);
    sockresource->pcb = pcb;

    // set up fd
    vfs_node_t* sock_node = malloc(sizeof(vfs_node_t));
    *sock_node = (vfs_node_t){
        .mountpoint = NULL,
        .redir = NULL,
        .resource = (resource_t*)sockresource,
        .filesystem = NULL,
        .name = "", // TODO: what name for socket?
        .parent = NULL,
        .children = NULL,

        // -1 means "not a directory", 0 is a directory with 0 children
        .children_count = -1,

        .symlink_target = NULL,
    };
    file_handle_t* sock_handle = malloc(sizeof(file_handle_t));
    *sock_handle = (file_handle_t){
        .resource = sock_node->resource,
        .node = sock_node,
        .refcount = 1
    };
    file_descriptor_t* sock_fd = malloc(sizeof(file_descriptor_t));
    *sock_fd = (file_descriptor_t){
        .handle = sock_handle,
    };

    process->fds[cur_fd] = sock_fd;
    lock_release(&process->fds_lock);
    // cast pointer to uint64_t for syscall signature match
    return (uint64_t)cur_fd;
}

uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t)
{
    process_t* process = get_current_thread()->process;
    if(fd >= PROC_MAX_FDS)
    {
        return -EINVAL;
    }
    file_descriptor_t* sockfd = process->fds[(int)fd];
    if(sockfd == NULL)
    {
        return -EBADF;
    }
    if((sockfd->handle->resource->stat.mode & STAT_IFMT) != STAT_IFSOCK)
    {
        return -ENOTSOCK;
    }
    socket_resource_t* sockresource = (socket_resource_t*)sockfd->handle->resource;
    if(sockresource->state == SOCK_STATE_BOUND)
    {
        return -EINVAL;
    }
    lock_acquire(&udp_port_table_lock);
    uint16_t port = ntohs(((net_addr_t*)addr)->port);
    if(udp_port_table[port] != NULL)
    {
        lock_release(&udp_port_table_lock);
        return -EADDRINUSE;
    }
    sockresource->local_addr = *((net_addr_t*)addr);
    udp_port_table[port] = sockresource;
    sockresource->state = SOCK_STATE_BOUND;
    lock_release(&udp_port_table_lock);
    return 0;
}

uint64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len,
                    uint64_t dest, uint64_t, uint64_t)
{
    process_t* process = get_current_thread()->process;
    if(fd >= PROC_MAX_FDS)
    {
        return -EINVAL;
    }
    file_descriptor_t* sockfd = process->fds[(int)fd];
    if(sockfd == NULL)
    {
        return -EBADF;
    }
    if((sockfd->handle->resource->stat.mode & STAT_IFMT) != STAT_IFSOCK)
    {
        return -ENOTSOCK;
    }
    net_addr_t* dest_addr = (net_addr_t*)dest;
    uint8_t* payload = (uint8_t*)buf;
    // ethernet, ipv4, UDP headers, plus payload
    size_t total_len = ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN + len;
    route_result_t route;
    if(!route_lookup(ntohl(dest_addr->ipv4), &route))
    {
        // TODO: error handling
        return -ENOSYS;
    }
    uint8_t next_hop_bytes[4];
    ip4_to_bytes(route.next_hop, next_hop_bytes);
    uint8_t dest_mac[6];
    if(!arp_lookup(route.device, next_hop_bytes, dest_mac))
    {
        // TODO: queue packet while arp request finishes
        return -ENOSYS;
    }
    uint8_t* packet = malloc(total_len);
    uint8_t dest_addr_bytes[4];
    ip4_to_bytes(ntohl(dest_addr->ipv4), dest_addr_bytes);
    eth_build_header(&packet[0], dest_mac, route.device->mac, ETHERTYPE_IPV4);
    ipv4_build_header(&packet[ETH_HEADER_LEN], route.device->ip4, dest_addr_bytes,
                           IP_PROTO_UDP, UDP_HEADER_LEN + len, DEFAULT_TTL);
    uint16_t source_port = ntohs(((socket_resource_t*)sockfd->handle->resource)->local_addr.port);
    if (source_port == 0)
    {
        uint16_t next_ephemeral = get_next_ephemeral_port();
        if(next_ephemeral == 0)
        {
            free(packet);
            return -EADDRINUSE;
        }
        source_port = next_ephemeral;
        socket_resource_t* sockresource = (socket_resource_t*)sockfd->handle->resource;
        sockresource->local_addr.port = htons(source_port);
        sockresource->state = SOCK_STATE_BOUND;
        udp_port_table[source_port] = sockresource;
    }
    udp_build_header(&packet[ETH_HEADER_LEN+IPV4_HEADER_LEN], source_port, ntohs(dest_addr->port), len);
    memcpy(&packet[ETH_HEADER_LEN+IPV4_HEADER_LEN+UDP_HEADER_LEN], payload, len);

    // finally we have the complete packet!
    route.device->transmit(packet, total_len);
    free(packet);

    return len;
}

uint64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len,
                      uint64_t src, uint64_t, uint64_t)
{
    (void)fd; (void)buf; (void)len; (void)src;
    return (uint64_t)-ENOSYS;
}

uint64_t sys_close_socket(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    (void)fd;
    return (uint64_t)-ENOSYS;
}

bool    socket_grow(resource_t* self, void* handle, uint64_t size)
{
    return false; // sockets can't grow
}

int64_t socket_read(resource_t* self, void* handle, void* buf, uint64_t loc, uint64_t count)
{
    // TODO: implement me
    return -ENOSYS;
}

int64_t socket_write(resource_t* self, void* handle, void* buf, uint64_t loc, uint64_t count)
{
    // TODO: implement me
    return -ENOSYS;
}

int     socket_ioctl(resource_t* self, void* handle, uint64_t request, void* argp)
{
    // TODO: implement me
    return -ENOSYS;
}

bool    socket_unref(resource_t* self, void* handle)
{
    // TODO: close the socket? I don't know if this call even makes sense
    return false;
}

bool    socket_link(resource_t* self, void* handle)
{
    // TODO: AF_UNIX will need an implementation here probably
    return false;
}

bool    socket_unlink(resource_t* self, void* handle)
{
    // TODO: AF_UNIX will need an implementation here probably
    return false;
}

void*   socket_mmap(resource_t* self, uint64_t page, int flags)
{
    // cannot mmap a socket
    return NULL;
}
