#include <net/network.h>
#include <klog/klog.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>
#include <errors/errno.h>
#include <mem/pmm.h>
#include <mem/malloc.h>
#include <stdatomic.h>

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
