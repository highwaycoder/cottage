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
