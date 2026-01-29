#pragma once

#include <stdint.h>

// Socket address families
enum SOCK_DOMAIN {
    AF_IPV4 = 1,
    AF_IPV6 = 2,
    AF_UNIX = 3,
};

// Socket types
enum SOCK_TYPE {
    SOCK_STREAM = 1,
    SOCK_DGRAM  = 2,
    SOCK_RAW    = 3,
};

// Socket protocols
enum SOCK_PROTOCOL {
    PROTO_UDP = 1,
    PROTO_TCP = 2,
};

// Network address structure
//
// BYTE ORDER CONVENTION: All address and port fields are stored in network byte
// order (big-endian). This matches the wire format and avoids repeated conversions
// when passing addresses between the socket layer, routing, and packet building.
//
// When populating from userspace, use htons()/htonl() on host-order values.
// When reading for display or host-side arithmetic, use ntohs()/ntohl().
// When copying from packet headers, use memcpy() to preserve wire order.
typedef struct {
    uint16_t family;    // AF_IPV4, AF_IPV6, etc.
    uint16_t port;      // Port number (network byte order)
    union {
        uint32_t ipv4;  // IPv4 address (network byte order)
        uint8_t  ipv6[16];
    };
} net_addr_t;
