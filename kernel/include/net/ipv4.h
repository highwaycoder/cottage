#pragma once

#include <stdint.h>
#include <net/network.h>
#include <net/endian.h>

// IPv4 header structure (follows 14-byte Ethernet header)
// Note: Options can extend the header beyond 20 bytes - check ihl field
typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;   // Version (4 bits) + Internet Header Length (4 bits)
    uint8_t  tos;           // Type of Service (usually 0)
    uint16_t total_length;  // Total packet length (header + data)
    uint16_t id;            // Identification (for fragmentation)
    uint16_t flags_frag;    // Flags (3 bits) + Fragment Offset (13 bits)
    uint8_t  ttl;           // Time To Live
    uint8_t  protocol;      // Protocol (1=ICMP, 6=TCP, 17=UDP)
    uint16_t checksum;      // Header checksum
    uint8_t  src_ip[4];     // Source IP address
    uint8_t  dest_ip[4];    // Destination IP address
    // Options may follow if ihl > 5
} ipv4_header_t;

// Protocol numbers
#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17
