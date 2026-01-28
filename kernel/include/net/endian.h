#pragma once

#include <stdint.h>

// 16-bit network byte order <-> host byte order
static inline uint16_t ntohs(uint16_t netshort)
{
    return (netshort >> 8) | (netshort << 8);
}

static inline uint16_t htons(uint16_t hostshort)
{
    return (hostshort >> 8) | (hostshort << 8);
}

// 32-bit network byte order <-> host byte order
static inline uint32_t ntohl(uint32_t netlong)
{
    return ((netlong >> 24) & 0xFF)
         | ((netlong >> 8)  & 0xFF00)
         | ((netlong << 8)  & 0xFF0000)
         | ((netlong << 24) & 0xFF000000);
}

static inline uint32_t htonl(uint32_t hostlong)
{
    return ((hostlong >> 24) & 0xFF)
         | ((hostlong >> 8)  & 0xFF00)
         | ((hostlong << 8)  & 0xFF0000)
         | ((hostlong << 24) & 0xFF000000);
}

// IPv4 address helpers (host byte order)
#define IP4(a, b, c, d) ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))

static inline void ip4_to_bytes(uint32_t ip, uint8_t out[4])
{
    out[0] = (ip >> 24) & 0xFF;
    out[1] = (ip >> 16) & 0xFF;
    out[2] = (ip >> 8)  & 0xFF;
    out[3] =  ip        & 0xFF;
}

static inline uint32_t ip4_from_bytes(uint8_t ip[4])
{
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16)
         | ((uint32_t)ip[2] << 8)  |  (uint32_t)ip[3];
}
