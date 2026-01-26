#include <net/network.h>
#include <net/ipv4.h>
#include <net/icmp.h>
#include <klog/klog.h>
#include <string.h>

void net_handle_icmp(network_device_t* device, uint8_t* packet, ipv4_header_t* ip,
                     uint8_t* payload, uint16_t payload_len)
{
    if (payload_len < sizeof(icmp_header_t))
    {
        klog("icmp", "Packet too short (%d bytes)", payload_len);
        return;
    }

    icmp_header_t* icmp = (icmp_header_t*)payload;

    klog("icmp", "ICMP type=%d code=%d", icmp->type, icmp->code);

    if (icmp->type == ICMP_ECHO_REQUEST)
    {
        klog("icmp", "Echo request (ping) from %d.%d.%d.%d, seq=%d",
            ip->src_ip[0], ip->src_ip[1], ip->src_ip[2], ip->src_ip[3],
            ntohs(icmp->sequence));

        // Build and send echo reply
        uint8_t reply[ETH_HEADER_LEN + IPV4_HEADER_LEN + payload_len];
        uint16_t off = 0;

        // 1. Build Ethernet header - use source MAC from incoming packet as destination
        uint8_t* incoming_src_mac = packet + 6;  // Ethernet source MAC is at offset 6
        off += eth_build_header(&reply[off], incoming_src_mac, device->mac, 0x0800);

        // 2. Build IPv4 header (swap src/dest IP)
        off += ipv4_build_header(&reply[off], device->ip4, ip->src_ip, IP_PROTO_ICMP, payload_len, 64);

        // 3. Build ICMP header (type=0, code=0, same id/sequence, copy payload data)
        icmp_header_t icmp_reply;
        icmp_reply.type = ICMP_ECHO_REPLY;
        icmp_reply.code = 0;
        icmp_reply.checksum = 0;  // Must be 0 for checksum calculation
        icmp_reply.id = icmp->id;
        icmp_reply.sequence = icmp->sequence;

        uint8_t* icmp_start = &reply[off];
        memcpy(icmp_start, &icmp_reply, sizeof(icmp_header_t));

        // Copy ping payload data after header
        memcpy(icmp_start + sizeof(icmp_header_t), payload + sizeof(icmp_header_t),
               payload_len - sizeof(icmp_header_t));

        // 4. Calculate ICMP checksum over ICMP header + data
        // Store in same byte order that net_checksum reads (little-endian on x86)
        uint16_t cksum = net_checksum(icmp_start, payload_len);
        icmp_start[2] = cksum & 0xFF;
        icmp_start[3] = (cksum >> 8) & 0xFF;

        // 5. Transmit
        device->transmit(reply, ETH_HEADER_LEN + IPV4_HEADER_LEN + payload_len);
        klog("icmp", "Sent echo reply to %d.%d.%d.%d",
             ip->src_ip[0], ip->src_ip[1], ip->src_ip[2], ip->src_ip[3]);
    }
}
