#pragma once

#include <stdint.h>
#include <net/network.h>
#include <net/ipv4.h>

// ICMP header structure
typedef struct __attribute__((packed)) {
    uint8_t  type;      // Message type
    uint8_t  code;      // Type-specific code
    uint16_t checksum;  // Checksum of ICMP header + data
    // Rest depends on type - for echo request/reply:
    uint16_t id;        // Identifier (usually process ID)
    uint16_t sequence;  // Sequence number
    // Followed by optional data (ping payload)
} icmp_header_t;

// ICMP types
#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8

// Handler function
void net_handle_icmp(network_device_t* device, uint8_t* packet, ipv4_header_t* ip,
                     uint8_t* payload, uint16_t payload_len);
