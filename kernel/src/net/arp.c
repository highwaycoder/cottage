#include <net/network.h>
#include <klog/klog.h>
#include <string.h>

// ARP cache
typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
} arp_entry_t;

#define ARP_CACHE_SIZE 32
static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static uint16_t last_arp_cache_entry;

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

// ARP opcodes
#define ARP_REQUEST 1
#define ARP_REPLY   2

// Helper to convert 16-bit network byte order to host
static inline uint16_t ntohs(uint16_t netshort)
{
    return (netshort >> 8) | (netshort << 8);
}

void net_handle_arp(network_device_t* device, uint8_t* packet, uint16_t len)
{
    // Minimum: 14 byte Ethernet header + 28 byte ARP header = 42 bytes
    if (len < 42)
    {
        klog("arp", "Packet too short (%d bytes)", len);
        return;
    }

    arp_header_t* arp = (arp_header_t*)(packet + 14);  // Skip Ethernet header

    uint16_t opcode = ntohs(arp->opcode);

    klog("arp", "ARP %s: %d.%d.%d.%d (%x:%x:%x:%x:%x:%x) -> %d.%d.%d.%d",
        opcode == ARP_REQUEST ? "request" : "reply",
        arp->sender_ip[0], arp->sender_ip[1], arp->sender_ip[2], arp->sender_ip[3],
        arp->sender_mac[0], arp->sender_mac[1], arp->sender_mac[2],
        arp->sender_mac[3], arp->sender_mac[4], arp->sender_mac[5],
        arp->target_ip[0], arp->target_ip[1], arp->target_ip[2], arp->target_ip[3]);

    if (opcode == ARP_REQUEST)
    {
        if (memcmp(arp->target_ip, device->ip4, 4) == 0)
        {
            // hand-craft an ethernet frame for the response
            uint8_t reply[42];
            memcpy(&reply[0], arp->sender_mac, 6);  // dest mac
            memcpy(&reply[6], device->mac, 6);      // src mac
            reply[12] = 0x08; reply[13] = 0x06;     // EtherType: ARP
            
            // ARP header
            reply[14] = 0x00; reply[15] = 0x01;     // Hardware type: Ethernet
            reply[16] = 0x08; reply[17] = 0x00;     // Protocol type: IPv4
            reply[18] = 6;                          // Hardware size (MAC addresses are 6 bytes long)
            reply[19] = 4;                          // Protocol size (IP addresses are 4 bytes long)
            reply[20] = 0x00; reply[21] = 0x02;     // Opcode: ARP_REPLY
            memcpy(&reply[22], device->mac, 6);     // Sender MAC (us)
            memcpy(&reply[28], device->ip4, 4);     // Sender IP  (us)
            memcpy(&reply[32], arp->sender_mac, 6);     // Target MAC (them)
            memcpy(&reply[38], arp->sender_ip, 4);     // Target IP  (them)

            device->transmit(reply, 42);
        }
    }
    else if (opcode == ARP_REPLY)
    {
        bool found = false;
        for(int i = 0; i < ARP_CACHE_SIZE; i++)
        {
            if (memcmp(arp_cache[i].ip, arp->sender_ip, 4) == 0)
            {
                memcpy(&arp_cache[i].mac, &arp->sender_mac, 6);
                found = true;
            }
        }
        if(!found && last_arp_cache_entry < ARP_CACHE_SIZE)
        {
            memcpy(&arp_cache[last_arp_cache_entry].ip, arp->sender_ip, 4);
            memcpy(&arp_cache[last_arp_cache_entry].mac, arp->sender_mac, 6);
            last_arp_cache_entry++;
        }
    }
}
