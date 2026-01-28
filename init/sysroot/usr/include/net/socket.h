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
typedef struct {
    uint16_t family;    // AF_IPV4, AF_IPV6, etc.
    uint16_t port;      // Port number (network byte order)
    union {
        uint32_t ipv4;
        uint8_t  ipv6[16];
    };
} net_addr_t;
