#pragma once

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

// flags
// is the device enabled?
#define NET_DEV_STATUS_ENABLE (1 << 0)
// is the device linked
#define NET_DEV_STATUS_LINK (1 << 1)
// is the device link ready?
#define NET_DEV_STATUS_LINK_READY (1 << 2)

typedef struct
{
    uint16_t length;
    uint16_t flags;
} packet_meta_t;

typedef struct 
{
    uint8_t* data; // ring of fixed-size slots
    packet_meta_t* meta; // parallel metadata ring
    uint32_t slot_count; // number of slots
    uint16_t slot_size; // size of each slot (hardcoded for now)
    _Atomic uint32_t head; // consumer reads from here
    _Atomic uint32_t tail; // producer writes here

    // TODO: signaling
} packet_queue_t;

#define NET_RECV_BUF_SLOT_SIZE 2048

typedef struct
{
    // the device's internal name (e.g "E1000")
    const char *name;
    ssize_t (*transmit)(uint8_t* data, uint16_t len);

    packet_queue_t recv_queue;

    // various control flags, used to control the device
    const uint8_t flags;

    // MAC address
    uint8_t mac[6];
} network_device_t;

typedef struct
{
    const char* identifier;
    network_device_t* device;
} network_device_descriptor_t;

// registers device <device> with identifier <identifier>,
// identifier will be used to name the /dev file, so must be a
// compliant filename without any special characters
void net_register_device(const char *identifier, network_device_t* device);

// initialises the network, if there are no devices registered,
// it will return false.
bool net_init();
ssize_t net_write(const char* devid, uint8_t* ptr, size_t len);
uint8_t* net_get_mac(const char* devid);
void packet_queue_init(packet_queue_t* queue, uint32_t slot_count);
