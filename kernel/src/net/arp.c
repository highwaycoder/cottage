#include <net/arp.h>
#include <net/network.h>
#include <net/endian.h>
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

// Internal: search the cache for a matching IP.
// Returns true and copies MAC to mac_out if found.
static bool arp_cache_lookup(uint8_t ip[4], uint8_t mac_out[6])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (memcmp(arp_cache[i].ip, ip, 4) == 0)
        {
            memcpy(mac_out, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

// Internal: broadcast an ARP request for target_ip.
static void arp_send_request(network_device_t* device, uint8_t target_ip[4])
{
    uint8_t request[42];

    // Ethernet header
    memset(&request[0], 0xFF, 6);               // dest: broadcast
    memcpy(&request[6], device->mac, 6);         // src: our MAC
    request[12] = 0x08; request[13] = 0x06;      // EtherType: ARP

    // ARP header
    request[14] = 0x00; request[15] = 0x01;      // Hardware type: Ethernet
    request[16] = 0x08; request[17] = 0x00;      // Protocol type: IPv4
    request[18] = 6;                              // Hardware size
    request[19] = 4;                              // Protocol size
    request[20] = 0x00; request[21] = 0x01;       // Opcode: ARP_REQUEST
    memcpy(&request[22], device->mac, 6);         // Sender MAC (us)
    memcpy(&request[28], device->ip4, 4);         // Sender IP (us)
    memset(&request[32], 0x00, 6);                // Target MAC (unknown)
    memcpy(&request[38], target_ip, 4);           // Target IP

    ssize_t sent = device->transmit(request, 42);
    klog("arp", "ARP request for %d.%d.%d.%d, transmit returned %d",
        target_ip[0], target_ip[1], target_ip[2], target_ip[3], (int)sent);
}

// Public: resolve an IP to a MAC address.
// Returns true on cache hit (mac_out filled), false on cache miss (ARP request sent).
bool arp_lookup(network_device_t* device, uint8_t ip[4], uint8_t mac_out[6])
{
    if (arp_cache_lookup(ip, mac_out))
    {
        return true;
    }

    arp_send_request(device, ip);
    return false;
}

// Called by the network thread when an ARP packet is received.
void net_handle_arp(network_device_t* device, uint8_t* packet, uint16_t len)
{
    if (len < 42)
    {
        klog("arp", "Packet too short (%d bytes)", len);
        return;
    }

    arp_header_t* arp = (arp_header_t*)(packet + 14);
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
            klog("arp", "ARP request is for us, sending reply");

            uint8_t reply[42];
            memcpy(&reply[0], arp->sender_mac, 6);
            memcpy(&reply[6], device->mac, 6);
            reply[12] = 0x08; reply[13] = 0x06;

            reply[14] = 0x00; reply[15] = 0x01;
            reply[16] = 0x08; reply[17] = 0x00;
            reply[18] = 6;
            reply[19] = 4;
            reply[20] = 0x00; reply[21] = 0x02;
            memcpy(&reply[22], device->mac, 6);
            memcpy(&reply[28], device->ip4, 4);
            memcpy(&reply[32], arp->sender_mac, 6);
            memcpy(&reply[38], arp->sender_ip, 4);

            ssize_t sent = device->transmit(reply, 42);
            klog("arp", "ARP reply transmit returned %d", (int)sent);
        }
        else
        {
            klog("arp", "ARP not for us (target %d.%d.%d.%d, we are %d.%d.%d.%d)",
                arp->target_ip[0], arp->target_ip[1], arp->target_ip[2], arp->target_ip[3],
                device->ip4[0], device->ip4[1], device->ip4[2], device->ip4[3]);
        }
    }
    else if (opcode == ARP_REPLY)
    {
        bool found = false;
        for (int i = 0; i < ARP_CACHE_SIZE; i++)
        {
            if (memcmp(arp_cache[i].ip, arp->sender_ip, 4) == 0)
            {
                memcpy(&arp_cache[i].mac, &arp->sender_mac, 6);
                found = true;
            }
        }
        if (!found && last_arp_cache_entry < ARP_CACHE_SIZE)
        {
            memcpy(&arp_cache[last_arp_cache_entry].ip, arp->sender_ip, 4);
            memcpy(&arp_cache[last_arp_cache_entry].mac, arp->sender_mac, 6);
            last_arp_cache_entry++;
        }
    }
}
