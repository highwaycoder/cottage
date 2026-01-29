#pragma once

#include <stdint.h>
#include <stdbool.h>

// Forward declaration
typedef struct network_device_s network_device_t;

// ARP header structure (follows 14-byte Ethernet header)
typedef struct __attribute__((packed)) {
    uint16_t hw_type;       // Hardware type (1 = Ethernet)
    uint16_t proto_type;    // Protocol type (0x0800 = IPv4)
    uint8_t  hw_size;       // Hardware address size (6 for Ethernet)
    uint8_t  proto_size;    // Protocol address size (4 for IPv4)
    uint16_t opcode;        // 1 = request, 2 = reply
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} arp_header_t;

#define ARP_REQUEST 1
#define ARP_REPLY   2

// Look up the MAC address for a given IPv4 address.
//
// On cache hit: copies the MAC into mac_out and returns true.
// On cache miss: sends an ARP request and returns false.
//   The caller should queue the packet and retry when the ARP reply arrives.
bool arp_lookup(network_device_t* device, uint8_t ip[4], uint8_t mac_out[6]);

// --- ARP pending packet queue ---
// Packets waiting for ARP resolution. Global ring buffer that overwrites
// oldest entries when full (no timeout needed, naturally self-evicting).

#define ARP_PENDING_QUEUE_SIZE 32

typedef struct {
    uint8_t* packet;       // full packet buffer (malloc'd), dest MAC at [0..5] unfilled
    uint16_t len;          // total packet length
    uint8_t  target_ip[4]; // IP we're waiting on ARP for (wire order)
    bool     occupied;     // true if this slot holds a valid pending packet
} arp_pending_entry_t;

// Initialise the pending packet queue (call once at boot)
void arp_pending_init(void);

// Enqueue a packet waiting for ARP resolution.
// Takes ownership of the packet buffer (caller must not free it).
// If the queue is full, the oldest entry is overwritten (and its buffer freed).
void arp_pending_enqueue(uint8_t* packet, uint16_t len, uint8_t target_ip[4]);

// Drain all pending packets whose target_ip matches resolved_ip.
// Fills in the destination MAC and transmits each one via the given device.
// Called from net_handle_arp when an ARP reply is received.
void arp_pending_drain(network_device_t* device, uint8_t resolved_ip[4], uint8_t resolved_mac[6]);
