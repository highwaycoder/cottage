#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <lock/lock.h>

// Forward declaration
typedef struct network_device_s network_device_t;

#define ROUTE_TABLE_SIZE 32

#define ROUTE_FLAG_UP      (1 << 0)  // Route is active
#define ROUTE_FLAG_GATEWAY (1 << 1)  // Route uses a gateway (next hop)
#define ROUTE_FLAG_HOST    (1 << 2)  // Route is to a specific host, not a network

typedef struct {
    uint32_t dest_network;       // Destination network (host byte order)
    uint32_t subnet_mask;        // Subnet mask (host byte order)
    uint32_t gateway;            // Next hop IP, 0 = on-link (host byte order)
    network_device_t* device;    // Outbound interface
    uint32_t metric;             // Lower = preferred
    uint32_t flags;
} route_entry_t;

typedef struct {
    route_entry_t* entry;        // Best matching route
    uint32_t next_hop;           // Resolved next hop: gateway IP, or dest IP if on-link
    network_device_t* device;    // Device to send through
} route_result_t;

// Initialise the routing table
void route_init(void);

// Add a route. Returns 0 on success, -errno on failure.
int route_add(uint32_t dest_network, uint32_t subnet_mask, uint32_t gateway,
              network_device_t* device, uint32_t metric, uint32_t flags);

// Remove a route matching dest_network and subnet_mask. Returns 0 on success, -errno on failure.
int route_remove(uint32_t dest_network, uint32_t subnet_mask);

// Look up the best route for a destination IP (host byte order).
// Returns true if a route was found, filling result. Returns false if no route matches.
bool route_lookup(uint32_t dest_ip, route_result_t* result);
