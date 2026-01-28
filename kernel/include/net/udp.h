#pragma once

#include <socket/socket.h>

void net_handle_udp(network_device_t* device, uint8_t* packet, ipv4_header_t* ip, uint8_t* payload, uint16_t payload_len);

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t payload_len;
    uint16_t checksum;
} udp_header_t;

extern lock_t udp_port_table_lock;
extern socket_resource_t* udp_port_table[65536];

