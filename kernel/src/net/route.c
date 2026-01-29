#include <net/arp.h>
#include <lock/lock.h>
#include <net/route.h>
#include <errors/errno.h>
#include <string.h>
#include <klog/klog.h>

static route_entry_t route_table[ROUTE_TABLE_SIZE];
static lock_t route_table_lock = LOCK_INITIALIZER("route_table");

void route_init(void)
{
    memset(route_table, 0, sizeof(route_table));
}

int route_add(uint32_t dest_network, uint32_t subnet_mask, uint32_t gateway,
              network_device_t* device, uint32_t metric, uint32_t flags)
{
    lock_acquire(&route_table_lock);

    // Find a free slot
    // scan route_table for an entry with ROUTE_FLAG_UP unset
    // If no free slot, unlock and return -ENOSPC
    // Fill in the entry fields and set ROUTE_FLAG_UP
    // If gateway is non-zero, also set ROUTE_FLAG_GATEWAY
    int free_slot = -1;
    for(int i = 0; i < ROUTE_TABLE_SIZE; i++)
    {
        route_entry_t* entry = &route_table[i];
        if((entry->flags & ROUTE_FLAG_UP) == 0)
        {
            free_slot = i;
            break;
        }
    }
    if(free_slot == -1)
    {
        lock_release(&route_table_lock);
        return -ENOSPC;
    }

    route_entry_t* entry = &route_table[free_slot];
    entry->dest_network = dest_network;
    entry->subnet_mask = subnet_mask;
    entry->gateway = gateway;
    entry->device = device;
    entry->metric = metric;
    entry->flags = flags | ROUTE_FLAG_UP | (entry->gateway ? ROUTE_FLAG_GATEWAY : 0);

    lock_release(&route_table_lock);
    return 0;
}

int route_remove(uint32_t dest_network, uint32_t subnet_mask)
{
    lock_acquire(&route_table_lock);

    // find matching entry (dest_network and subnet_mask both match)
    // Clear ROUTE_FLAG_UP (or memset the entry to 0)
    // If not found, unlock and return -ENOENT
    for(int i = 0; i < ROUTE_TABLE_SIZE; i++)
    {
        route_entry_t* entry = &route_table[i];
        if(entry->subnet_mask == subnet_mask && entry->dest_network == dest_network)
        {
            entry->flags &= ~ROUTE_FLAG_UP;
            lock_release(&route_table_lock);
            return 0;
        }
    }

    lock_release(&route_table_lock);
    return -ENOENT;
}

bool route_lookup(uint32_t dest_ip, route_result_t* result)
{
    lock_acquire(&route_table_lock);

    // Longest prefix match:
    // iterate all entries where ROUTE_FLAG_UP is set
    //   - Check if (dest_ip & entry->subnet_mask) == entry->dest_network
    //   - Track the match with the longest mask (most bits set = most specific)
    //   - On tie (same mask length), prefer lower metric
    //
    // If a match is found, fill result:
    //   - result->entry = best match
    //   - result->device = best->device
    //   - result->next_hop = best->gateway if ROUTE_FLAG_GATEWAY set,
    //                        otherwise dest_ip (on-link, ARP for destination directly)

    int best_match_weight = -1;
    int best_match = -1;
    for(int i = 0; i < ROUTE_TABLE_SIZE; i++)
    {
        route_entry_t* entry = &route_table[i];
        // skip entries that are not up
        if((entry->flags & ROUTE_FLAG_UP) == 0) continue;
        if((dest_ip & entry->subnet_mask) == entry->dest_network)
        {
            int mask_hamming_weight = __builtin_popcount(entry->subnet_mask);
            if(mask_hamming_weight > best_match_weight) {
                best_match = i;
                best_match_weight = mask_hamming_weight;
            }
            if(mask_hamming_weight == best_match_weight)
            {
                // TODO: metric tie-breaking
            }
        }
    }

    if(best_match == -1)
    {
        lock_release(&route_table_lock);
        return false;
    }

    result->entry = &route_table[best_match];
    result->device = route_table[best_match].device;
    if((route_table[best_match].flags & ROUTE_FLAG_GATEWAY) == 0)
    {
        result->next_hop = dest_ip;
    }
    else
    {
        result->next_hop = route_table[best_match].gateway;
    }

    lock_release(&route_table_lock);
    return true;
}
