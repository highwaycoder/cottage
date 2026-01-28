#pragma once

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <scheduler/semaphore.h>

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

    // signaling
    semaphore_t packet_ready;
} packet_queue_t;

#define NET_RECV_BUF_SLOT_SIZE 2048

typedef struct network_device_s
{
    // the device's internal name (e.g "E1000")
    const char *name;
    ssize_t (*transmit)(uint8_t* data, uint16_t len);

    packet_queue_t recv_queue;

    // various control flags, used to control the device
    const uint8_t flags;

    // MAC address
    uint8_t mac[6];

    // IPv4 address (single IPv4 address per device, *for now*)
    uint8_t ip4[4];
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

// the kernel network thread, defined in network.c for now
void knetwork_thread(void* arg);

// Protocol handlers
void net_handle_arp(network_device_t* device, uint8_t* packet, uint16_t len);
void net_handle_ipv4(network_device_t* device, uint8_t* packet, uint16_t len);

// Packet building helpers
// All return the number of bytes written (header size)

// Build Ethernet header at buf[0..13]
// Returns 14 (ETH_HEADER_LEN)
uint16_t eth_build_header(uint8_t* buf, uint8_t* dest_mac, uint8_t* src_mac, uint16_t ethertype);

// Build IPv4 header at buf[0..19] (no options)
// Checksum is calculated automatically
// Returns 20 (minimum IPv4 header size)
uint16_t ipv4_build_header(uint8_t* buf, uint8_t* src_ip, uint8_t* dest_ip,
                           uint8_t protocol, uint16_t payload_len, uint8_t ttl);

// Build UDP header at buf[0..7] (no options)
// Returns 8 (UDP_HEADER_LEN)
uint16_t udp_build_header(uint8_t* buf, uint16_t src_port, uint16_t dst_port, uint16_t payload_len);

// Internet checksum (RFC 1071) - used for IPv4 header, ICMP, UDP, etc.
uint16_t net_checksum(void* data, uint16_t len);

// Header sizes
#define ETH_HEADER_LEN  14
#define IPV4_HEADER_LEN 20  // Minimum, without options
#define UDP_HEADER_LEN 8

// EtherType
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

// TTL
#define DEFAULT_TTL 64

// Ephemeral port range
#define EPHEMERAL_PORT_MIN 49152
#define EPHEMERAL_PORT_MAX 65535
