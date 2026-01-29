#include <klog/klog.h>
#include <net/network.h>
#include <net/ipv4.h>
#include <net/udp.h>
#include <net/endian.h>
#include <mem/malloc.h>
#include <string.h>

lock_t udp_port_table_lock = LOCK_INITIALIZER("udp_port_table_lock");
socket_resource_t* udp_port_table[65536] = {0};

void net_handle_udp(network_device_t* device, uint8_t* packet, ipv4_header_t* ip, uint8_t* payload, uint16_t payload_len)
{
    if(payload_len < 8)
    {
        klog_debug("udp", "Invalid UDP packet received, payload_len < 8");
        return;
    }
    udp_header_t* udp_header = (udp_header_t*)payload;
    uint16_t dest_port = ntohs(udp_header->dest_port);
    socket_resource_t* socket_resource = udp_port_table[dest_port];
    if(socket_resource == NULL)
    {
        klog_debug("udp", "UDP dest port unreachable: %d", dest_port);
        return;
    }

    udp_pcb_t* pcb = (udp_pcb_t*)(socket_resource->pcb);
    if((pcb->recv_queue_tail + 1) % RECV_QUEUE_SIZE == pcb->recv_queue_head)
    {
        klog_debug("udp", "UDP receive buffer full");
        return;
    }
    datagram_t* datagram = malloc(sizeof(datagram_t) + payload_len - 8);
    memcpy(&datagram->remote_addr.ipv4, ip->src_ip, 4);
    datagram->remote_addr.family = AF_IPV4;
    datagram->remote_addr.port = udp_header->src_port;
    datagram->len = payload_len - 8;
    memcpy(datagram->payload, payload + 8, payload_len - 8);

    pcb->recv_queue[pcb->recv_queue_tail] = datagram;
    pcb->recv_queue_tail = (pcb->recv_queue_tail + 1) % RECV_QUEUE_SIZE;
    sem_signal(&pcb->sem);
}
