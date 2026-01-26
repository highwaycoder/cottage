#include <net/network.h>
#include <klog/klog.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>
#include <errors/errno.h>
#include <mem/pmm.h>
#include <mem/malloc.h>
#include <stdatomic.h>
#include <scheduler/semaphore.h>
#include <scheduler/scheduler.h>

// Internet checksum (RFC 1071)
// Works for IPv4 header, ICMP, UDP, TCP, etc.
uint16_t net_checksum(void* data, uint16_t len)
{
    uint32_t sum = 0;
    uint16_t* ptr = (uint16_t*)data;

    while (len > 1)
    {
        sum += *ptr++;
        len -= 2;
    }

    // Handle odd byte
    if (len == 1)
    {
        sum += *(uint8_t*)ptr;
    }

    // Fold 32-bit sum to 16 bits
    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

// Build Ethernet header
uint16_t eth_build_header(uint8_t* buf, uint8_t* dest_mac, uint8_t* src_mac, uint16_t ethertype)
{
    memcpy(&buf[0], dest_mac, 6);    // Destination MAC
    memcpy(&buf[6], src_mac, 6);     // Source MAC
    buf[12] = (ethertype >> 8) & 0xFF;  // EtherType high byte
    buf[13] = ethertype & 0xFF;         // EtherType low byte
    return ETH_HEADER_LEN;
}

// Build IPv4 header (no options, 20 bytes)
uint16_t ipv4_build_header(uint8_t* buf, uint8_t* src_ip, uint8_t* dest_ip,
                           uint8_t protocol, uint16_t payload_len, uint8_t ttl)
{
    uint16_t total_len = IPV4_HEADER_LEN + payload_len;

    buf[0] = 0x45;                      // Version 4, IHL 5 (20 bytes)
    buf[1] = 0x00;                      // TOS
    buf[2] = (total_len >> 8) & 0xFF;   // Total length high
    buf[3] = total_len & 0xFF;          // Total length low
    buf[4] = 0x00;                      // Identification high
    buf[5] = 0x00;                      // Identification low
    buf[6] = 0x40;                      // Flags: Don't Fragment, frag offset high
    buf[7] = 0x00;                      // Fragment offset low
    buf[8] = ttl;                       // TTL
    buf[9] = protocol;                  // Protocol
    buf[10] = 0x00;                     // Checksum high (placeholder)
    buf[11] = 0x00;                     // Checksum low (placeholder)
    memcpy(&buf[12], src_ip, 4);        // Source IP
    memcpy(&buf[16], dest_ip, 4);       // Destination IP

    // Calculate and insert checksum
    // Store in same byte order that net_checksum reads (little-endian on x86)
    uint16_t cksum = net_checksum(buf, IPV4_HEADER_LEN);
    buf[10] = cksum & 0xFF;
    buf[11] = (cksum >> 8) & 0xFF;

    return IPV4_HEADER_LEN;
}

static network_device_descriptor_t* devices = NULL;
static size_t device_count;

uint8_t* net_get_mac(const char* devid)
{

    for(size_t i = 0; i < device_count; i++)
    {
        if(strcmp(devices[i].identifier, devid) == 0)
        {
            return devices[i].device->mac;
        }
    }
    return NULL;
}

void packet_queue_init(packet_queue_t* queue, uint32_t slot_count)
{
    queue->data = (uint8_t*)((uintptr_t)pmm_alloc((slot_count * NET_RECV_BUF_SLOT_SIZE) / PAGE_SIZE) + HIGHER_HALF);
    queue->meta = malloc(slot_count * sizeof(packet_meta_t));
    queue->slot_count = slot_count;
    queue->slot_size = NET_RECV_BUF_SLOT_SIZE;

    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);

    sem_init(&queue->packet_ready, 0);
}

void net_register_device(const char *identifier, network_device_t* device)
{
    klog("net", "Registering networking device %s", identifier);

    // step 1: grow the device array
    device_count++;
    devices = realloc(devices, device_count * sizeof(network_device_descriptor_t));

    // step 2: insert the device into the end of the array
    devices[device_count - 1] = (network_device_descriptor_t) {
        .device = device,
        .identifier = identifier
    };

    // step 3: spawn a consumer thread for this device
    new_kernel_thread(knetwork_thread, device, true);
}

// very low-level call, simply writes <len> bytes to the network device's
// send buffer.  Used by higher level APIs to implement protocol writes.
// it is not expected for typical users to be using this function directly.
// returns the number of bytes written (if <= len, it means the write was
// truncated, possibly due to lack of buffer space)
ssize_t net_write(const char* devid, uint8_t* ptr, size_t len)
{
    for(size_t i = 0; i < device_count; i++)
    {
        if(strcmp(devices[i].identifier, devid) == 0)
        {
            return devices[i].device->transmit(ptr, len);
        }
    }
    return -ENODEV;
}

// this function is responsible for initialising the network protocol stack,
// and is intended to be run as a task in the OS scheduler.
bool net_init()
{
    if(device_count == 0) return false;

    return true;
}

void knetwork_thread(void* arg)
{
    network_device_t* device = (network_device_t*)arg;
    packet_queue_t* queue = &device->recv_queue;

    klog("net", "Network thread started for device %s", device->name);

    while (1)
    {
        // Block until a packet is available
        sem_wait(&queue->packet_ready);

        // Dequeue the packet
        // We're the only consumer, so head won't move out from under us
        uint32_t head = atomic_load(&queue->head);
        uint32_t tail = atomic_load(&queue->tail);

        if (head == tail)
        {
            // Spurious wakeup or race - no packet actually available
            // This shouldn't happen with correct semaphore usage, but defensive
            klog("net", "Warning: woke up but queue empty");
            continue;
        }

        // Get packet data and metadata from the head slot
        uint8_t* packet_data = queue->data + (head * queue->slot_size);
        uint16_t packet_len = queue->meta[head].length;

        // Advance head (we're done with this slot)
        uint32_t next_head = (head + 1) % queue->slot_count;
        atomic_store(&queue->head, next_head);

        // EtherType is at bytes 12-13 (big-endian)
        uint16_t ethertype = (packet_data[12] << 8) | packet_data[13];

        // Dispatch to protocol handler
        switch (ethertype)
        {
            case 0x0806:  // ARP
                net_handle_arp(device, packet_data, packet_len);
                break;
            case 0x0800:  // IPv4
                net_handle_ipv4(device, packet_data, packet_len);
                break;
            default:
                klog("net", "Unknown EtherType 0x%x, dropping %d byte packet", ethertype, packet_len);
                break;
        }
    }
}
