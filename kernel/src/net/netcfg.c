#include <net/netcfg.h>
#include <net/route.h>
#include <net/network.h>
#include <net/endian.h>
#include <klog/klog.h>

void netcfg_init(void)
{
    // TODO: eventually read from a config file on the filesystem
    // For now, hardcode the QEMU default network setup:
    //
    // Device eth0: 10.0.2.15/24
    // Local subnet: 10.0.2.0/24 via eth0 (on-link)
    // Default route: 0.0.0.0/0 via gateway 10.0.2.2
    //
    network_device_t* eth0 = net_get_device("eth0");
    if (eth0 == NULL) {
        klog("netcfg", "eth0 not found, skipping network configuration");
        return;
    }
    
    // 10.0.2.0/24 via eth0, on-link
    route_add(htonl(0x0A000200), htonl(0xFFFFFF00), 0, eth0, 0,
               ROUTE_FLAG_UP);
    // 0.0.0.0/24 via eth0, gateway 10.0.2.2
    route_add(0, 0, htonl(0x0A000202), eth0, 100,
               ROUTE_FLAG_UP | ROUTE_FLAG_GATEWAY);
    klog("netcfg", "network configuration applied");
}
