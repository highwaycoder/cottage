#include <net/network.h>
#include <net/ipv4.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <klog/klog.h>
#include <string.h>

void net_handle_ipv4(network_device_t* device, uint8_t* packet, uint16_t len)
{
    // Minimum: 14 byte Ethernet + 20 byte IPv4 header = 34 bytes
    if (len < 34)
    {
        klog("ipv4", "Packet too short (%d bytes)", len);
        return;
    }

    ipv4_header_t* ip = (ipv4_header_t*)(packet + 14);  // Skip Ethernet header

    // Extract version and header length
    uint8_t version = (ip->version_ihl >> 4) & 0x0F;
    uint8_t ihl = ip->version_ihl & 0x0F;  // Header length in 32-bit words
    uint8_t header_len = ihl * 4;          // Header length in bytes

    if (version != 4)
    {
        klog("ipv4", "Not IPv4 (version=%d)", version);
        return;
    }

    if(memcmp(ip->dest_ip, device->ip4, 4) != 0)
    {
        if(memcmp(ip->dest_ip, (uint8_t[]){255,255,255,255}, 4) == 0)
        {
            // we log and ignore broadcast messages for now, as I'm not sure what we should actually do here
            klog("ipv4", "Broadcast received, ignoring (for now)");
        }
        // early return, packet isn't for us
        return;
    }

    // TODO: we trust that the NIC has verified the checksum, for now, but in the future we will want to do our own
    // verification to catch hardware bugs/issues

    // Calculate where payload starts and its length
    uint8_t* payload = ((uint8_t*)ip) + header_len;
    uint16_t total_len = ntohs(ip->total_length);
    uint16_t payload_len = total_len - header_len;

    klog("ipv4", "IPv4 %d.%d.%d.%d -> %d.%d.%d.%d proto=%d len=%d",
        ip->src_ip[0], ip->src_ip[1], ip->src_ip[2], ip->src_ip[3],
        ip->dest_ip[0], ip->dest_ip[1], ip->dest_ip[2], ip->dest_ip[3],
        ip->protocol, payload_len);

    // Dispatch based on protocol
    switch (ip->protocol)
    {
        case IP_PROTO_ICMP:
            net_handle_icmp(device, packet, ip, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            klog("ipv4", "TCP not yet implemented");
            break;
        case IP_PROTO_UDP:
            net_handle_udp(device, packet, ip, payload, payload_len);
            break;
        default:
            klog("ipv4", "Unknown protocol %d", ip->protocol);
            break;
    }
}
