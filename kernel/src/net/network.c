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
    queue->data = pmm_alloc((slot_count * NET_RECV_BUF_SLOT_SIZE) / PAGE_SIZE);
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
        klog("net", "about to sem_wait on %p", &queue->packet_ready);
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

        // Process the packet - for now just log it
        // EtherType is at bytes 12-13 (big-endian)
        uint16_t ethertype = (packet_data[12] << 8) | packet_data[13];
        klog("net", "RX: %d bytes, EtherType=0x%x", packet_len, ethertype);

        // TODO: dispatch to protocol handlers based on ethertype
        // 0x0806 = ARP
        // 0x0800 = IPv4
        // 0x86DD = IPv6
    }
}
