#include <pci/pci.h>
#include <stdint.h>
#include <io/io.h>
#include <mem/pagemap.h>
#include <lock/lock.h>
#include <klog/klog.h>
#include <acpi/acpi.h>
#include <stddef.h>
#include <debug/debug.h>
#include <drivers/e1000/e1000.h>
#include <panic.h>
#include <time/timer.h>

#define COMMAND_PORT 0xcf8
#define DATA_PORT 0xcfc

extern pagemap_t *g_kernel_pagemap;

const char *device_classes[] = {
    "Unclassified",
    "Mass Storage Controller",
    "Network Controller",
    "Display Controller",
    "Multimedia Controller",
    "Memory Controller",
    "Bridge Device",
    "Simple Communication Controller",
    "Base System Peripheral",
    "Input Device Controller",
    "Docking Station",
    "Processor",
    "Serial Bus Controller",
    "Wireless Controller",
    "Intelligent Controller",
    "Satellite Communication Controller",
    "Encryption Controller",
    "Signal Processing Controller",
    "Processing Accelerator",
    "Non Essential Instrumentation"};

uint8_t pci_readb(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset)
{
    uint32_t id = 0x1 << 31 | ((bus & 0xFF) << 16) | ((device & 0x1F) << 11) | ((function & 0x07) << 8) | (registeroffset & 0xFC);

    outd(COMMAND_PORT, id);
    uint8_t result = inb(DATA_PORT);
    return result >> (8 * (registeroffset % 4));
}

uint16_t pci_readw(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset)
{
    uint32_t id = 0x1 << 31 | ((bus & 0xFF) << 16) | ((device & 0x1F) << 11) | ((function & 0x07) << 8) | (registeroffset & 0xFC);

    outd(COMMAND_PORT, id);
    uint16_t result = inw(DATA_PORT);
    return result >> (8 * (registeroffset % 4));
}

uint32_t pci_readd(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset)
{
    uint32_t id = 0x1 << 31 | ((bus & 0xFF) << 16) | ((device & 0x1F) << 11) | ((function & 0x07) << 8) | (registeroffset & 0xFC);

    outd(COMMAND_PORT, id);
    uint32_t result = ind(DATA_PORT);
    return result >> (8 * (registeroffset % 4));
}

void pci_writeb(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset, uint8_t val)
{
    uint32_t id = 0x1 << 31 | ((bus & 0xFF) << 16) | ((device & 0x1F) << 11) | ((function & 0x07) << 8) | (registeroffset & 0xFC);
    outd(COMMAND_PORT, id);
    outb(DATA_PORT, val);
}

void pci_writew(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset, uint16_t val)
{
    uint32_t id = 0x1 << 31 | ((bus & 0xFF) << 16) | ((device & 0x1F) << 11) | ((function & 0x07) << 8) | (registeroffset & 0xFC);
    outd(COMMAND_PORT, id);
    outw(DATA_PORT, val);
}

void pci_writed(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset, uint32_t val)
{
    uint32_t id = 0x1 << 31 | ((bus & 0xFF) << 16) | ((device & 0x1F) << 11) | ((function & 0x07) << 8) | (registeroffset & 0xFC);
    outd(COMMAND_PORT, id);
    outd(DATA_PORT, val);
}

// PCI enumeration runs during single-threaded boot
NO_THREAD_SAFETY_ANALYSIS
void enumerate_function(uint64_t address, uint64_t function, __attribute__((unused)) uint16_t bus, __attribute__((unused)) uint16_t device)
{
    uint64_t offset = function << 12;

    uint64_t function_address = address + offset;

    map_page(g_kernel_pagemap, function_address, function_address, PTE_FLAG_PRESENT | PTE_FLAG_WRITABLE);

    pci_header_0_t *pci_device_header = (pci_header_0_t *)function_address;

    if (pci_device_header->header.device_id == 0)
    {
        return;
    }
    if (pci_device_header->header.device_id == 0xffff)
    {
        return;
    }

    const char *vendor_name = get_vendor_name(pci_device_header->header.vendor_id);
    const char *device_name = get_device_name(pci_device_header->header.vendor_id, pci_device_header->header.device_id);
    const char *subclass_name = get_subclass_name(pci_device_header->header.class_, pci_device_header->header.subclass);
    const char *prog_IF_name = get_prog_IF_name(pci_device_header->header.class_, pci_device_header->header.subclass, pci_device_header->header.prog_if);

    klog("pci", "Found device:");
    if (vendor_name != NULL)
        klog("pci", "\tVendor name: %s", vendor_name);
    else
        klog("pci", "\tVendor id  : %x", pci_device_header->header.vendor_id);

    if (device_name != NULL)
        klog("pci", "\tDevice name: %s", device_name);
    else
        klog("pci", "\tDevice id  : %x", pci_device_header->header.device_id);

    klog("pci", "\tDevice class: %s, ", device_classes[pci_device_header->header.class_]);

    if (subclass_name != NULL)
        klog("pci", "\tSub class name: %s, ", subclass_name);
    else
        klog("pci", "\tsub class id  : %x, ", pci_device_header->header.subclass);

    if (prog_IF_name != NULL)
        klog("pci", "\tProg interface name: %s\n", prog_IF_name);
    else
        klog("pci", "\tProg interface id  : %x\n", pci_device_header->header.prog_if);

    // this is hacky, I don't like this
    if (pci_device_header->header.vendor_id == E1000_PCI_VENDOR &&
        pci_device_header->header.device_id == E1000_PCI_DEVICE
    ) {
        klog("pci", "Found E1000 Network Card, initializing driver");
        bool init = false;

        pci_enable_mmio(bus, device, function);
        // in other OSes, this is done after initialisation of the driver
        // maybe there's a reason for this?  If you get bugs, check here.
        pci_become_bus_master(bus, device, function);

        for(size_t i = 0; i < 6; i++)
        {
            pci_bar_t bar = pci_get_bar(&pci_device_header->BAR0, i, bus, device, function);
            if(bar.type == PCI_BAR_TYPE_MMIO32 || bar.type == PCI_BAR_TYPE_MMIO64)
            {
                
                e1000_init(bar.mem_address);
                init = true;
                break;
            }
            else if(bar.type == PCI_BAR_TYPE_IO)
            {
                // not implementing the I/O version of E1000 for simplicity
                klog("pci", "Warning: network card I/O mode being ignored");
                break;
            }
        }

        if(init)
            klog("pci", "E1000 Network Card Driver initialized");
        else
            panic("No network card!"); // for now this is the only driver we have
    }
}

// enables MMIO on this device (by allowing memory space accesses)
void pci_enable_mmio(uint16_t bus, uint16_t device, uint16_t function)
{
    pci_writed(bus, device, function, 0x4, pci_readd(bus, device, function, 0x4) | (1 << 1));
}

// allows this device to generate PCI accesses
void pci_become_bus_master(uint16_t bus, uint16_t device, uint16_t function)
{
    pci_writed(bus, device, function, 0x4, pci_readd(bus, device, function, 0x4) | (1 << 2));
}

void read_bar(uint32_t* mask, uint16_t bus, uint16_t device, uint16_t function, uint32_t offset) {
	uint32_t data = pci_readd(bus, device, function, offset);
	pci_writed(bus, device, function, offset, 0xffffffff);
	*mask = pci_readd(bus, device, function, offset);
	pci_writed(bus, device, function, offset, data);
}

pci_bar_t pci_get_bar(uint32_t* bar0, int bar_num, uint16_t bus, uint16_t device, uint16_t function)
{
    pci_bar_t bar;
	uint32_t* bar_ptr = (uint32_t*) (bar0 + bar_num * sizeof(uint32_t));

	if (*bar_ptr) {
		uint32_t mask;
		read_bar(&mask, bus, device, function, bar_num * sizeof(uint32_t));

		if (*bar_ptr & 0x04) { //64-bit mmio
			bar.type = PCI_BAR_TYPE_MMIO64;

			uint32_t* bar_ptr_high = (uint32_t*) (bar0 + bar_num * sizeof(uint32_t));
			uint32_t mask_high;
			read_bar(&mask_high, bus, device, function, (bar_num * sizeof(uint32_t)) + 0x4);

			bar.mem_address = ((uint64_t) (*bar_ptr_high & ~0xf) << 32) | (*bar_ptr & ~0xf);
			bar.size = (((uint64_t) mask_high << 32) | (mask & ~0xf)) + 1;
		} else if (*bar_ptr & 0x01) { //IO
			bar.type = PCI_BAR_TYPE_IO;

			bar.io_address = (uint16_t)(*bar_ptr & ~0x3);
			bar.size = (uint16_t)(~(mask & ~0x3) + 1);
		} else { //32-bit mmio
			bar.type = PCI_BAR_TYPE_MMIO32;

			bar.mem_address = (uint64_t) *bar_ptr & ~0xf;
			bar.size = ~(mask & ~0xf) + 1;
		}
	} else {
		bar.type = PCI_BAR_TYPE_NONE;
	}

	return bar;
}

// PCI enumeration runs during single-threaded boot
NO_THREAD_SAFETY_ANALYSIS
void enumerate_device(uint64_t bus_address, uint64_t device, uint16_t bus)
{
    uint64_t offset = device << 15;

    uint64_t device_address = bus_address + offset;

    map_page(g_kernel_pagemap, device_address, device_address, PTE_FLAG_PRESENT | PTE_FLAG_WRITABLE);

    pci_device_header_t *pci_device_header = (pci_device_header_t *)device_address;

    if (pci_device_header->device_id == 0)
    {
        return;
    }
    if (pci_device_header->device_id == 0xffff)
    {
        return;
    }

    for (uint64_t function = 0; function < 8; function++)
    {
        enumerate_function(device_address, function, bus, device);
    }
}

// PCI enumeration runs during single-threaded boot
NO_THREAD_SAFETY_ANALYSIS
void enumerate_bus(uint64_t base_address, uint64_t bus)
{
    uint64_t offset = bus << 20;

    uint64_t bus_address = base_address + offset;
    map_page(g_kernel_pagemap, bus_address, bus_address, PTE_FLAG_PRESENT | PTE_FLAG_WRITABLE);

    pci_device_header_t *pci_device_header = (pci_device_header_t *)bus_address;

    if (pci_device_header->device_id == 0)
    {
        return;
    }
    if (pci_device_header->device_id == 0xffff)
    {
        return;
    }

    for (uint64_t device = 0; device < 32; device++)
    {
        enumerate_device(bus_address, device, bus);
    }
}

void pci_init()
{
    mcfg_header_t *mcfg_table = (mcfg_header_t *)acpi_find_table("MCFG", 0);

    int entries = (mcfg_table->header.length -
                   sizeof(mcfg_header_t)) /
                  sizeof(mcfg_device_config_t);
    klog("pci", "Found %d MCFG table entries", entries);
    for (int i = 0; i < entries; i++)
    {
        mcfg_device_config_t *device_config = (mcfg_device_config_t *)(((uint64_t)mcfg_table) + sizeof(mcfg_header_t) + (sizeof(mcfg_device_config_t) * i));
        klog("pci", "Enumerating busses between %d and %d", device_config->start_pci_bus, device_config->end_pci_bus);
        for (size_t bus = device_config->start_pci_bus; bus < device_config->end_pci_bus; bus++)
        {
            enumerate_bus(device_config->base_address, bus);
        }
    }
}

const char *get_vendor_name(uint16_t vendor_ID)
{
    switch (vendor_ID)
    {
    case 0x8086:
        return "Intel Corp.";
    case 0x1022:
        return "AMD, Inc.";
    case 0x10DE:
        return "NVIDIA Corporation";
    case 0x10EC:
        return "Realtek Semiconductor Co., Ltd.";
    }
    return NULL;
}

const char *get_device_name(uint16_t vendor_ID, uint16_t device_ID)
{
    switch (vendor_ID)
    {
    case 0x8086: // Intel
        switch (device_ID)
        {
        case 0x29C0:
            return "Express DRAM Controller";
        case 0x2918:
            return "LPC Interface Controller";
        case 0x2922:
            return "6 port SATA Controller [AHCI mode]";
        case 0x2930:
            return "SMBus Controller";
        case 0x100E:
            return "Intel Gigabit Ethernet"; // Qemu, Bochs, and VirtualBox emmulated NICs
        case 0x10EA:
            return "82577LM Gigabit Network Connection";
        case 0x153A:
            return "Ethernet Connection I217-LM";
        }
    case 0x1022: // AMD
        switch (device_ID)
        {
        case 0x2000:
            return "AM79C973";
        }
    case 0x10EC: // Realtek
        switch (device_ID)
        {
        case 0x8139:
            return "RTL8193";
        }
    }
    return NULL;
}

const char *mass_storage_controller_subclass_name(uint8_t subclass_code)
{
    switch (subclass_code)
    {
    case 0x00:
        return "SCSI Bus Controller";
    case 0x01:
        return "IDE Controller";
    case 0x02:
        return "Floppy Disk Controller";
    case 0x03:
        return "IPI Bus Controller";
    case 0x04:
        return "RAID Controller";
    case 0x05:
        return "ATA Controller";
    case 0x06:
        return "Serial ATA";
    case 0x07:
        return "Serial Attached SCSI";
    case 0x08:
        return "Non-Volatile Memory Controller";
    case 0x80:
        return "Other";
    }
    return NULL;
}

const char *serial_bus_controller_subclass_name(uint8_t subclass_code)
{
    switch (subclass_code)
    {
    case 0x00:
        return "FireWire (IEEE 1394) Controller";
    case 0x01:
        return "ACCESS Bus";
    case 0x02:
        return "SSA";
    case 0x03:
        return "USB Controller";
    case 0x04:
        return "Fibre Channel";
    case 0x05:
        return "SMBus";
    case 0x06:
        return "Infiniband";
    case 0x07:
        return "IPMI Interface";
    case 0x08:
        return "SERCOS Interface (IEC 61491)";
    case 0x09:
        return "CANbus";
    case 0x80:
        return "SerialBusController - Other";
    }
    return NULL;
}

const char *bridge_device_subclass_name(uint8_t subclass_code)
{
    switch (subclass_code)
    {
    case 0x00:
        return "Host Bridge";
    case 0x01:
        return "ISA Bridge";
    case 0x02:
        return "EISA Bridge";
    case 0x03:
        return "MCA Bridge";
    case 0x04:
        return "PCI-to-PCI Bridge";
    case 0x05:
        return "PCMCIA Bridge";
    case 0x06:
        return "NuBus Bridge";
    case 0x07:
        return "CardBus Bridge";
    case 0x08:
        return "RACEway Bridge";
    case 0x09:
        return "PCI-to-PCI Bridge";
    case 0x0a:
        return "InfiniBand-to-PCI Host Bridge";
    case 0x80:
        return "Other";
    }
    return NULL;
}

const char *get_subclass_name(uint8_t class_code, uint8_t subclass_code)
{
    switch (class_code)
    {
    case 0x01:
        return mass_storage_controller_subclass_name(subclass_code);
    case 0x03:
        switch (subclass_code)
        {
        case 0x00:
            return "VGA Compatible Controller";
        }
    case 0x06:
        return bridge_device_subclass_name(subclass_code);
    case 0x0C:
        return serial_bus_controller_subclass_name(subclass_code);
    }
    return NULL;
}
const char *get_prog_IF_name(uint8_t class_code, uint8_t subclass_code, uint8_t prog_IF)
{
    switch (class_code)
    {
    case 0x01:
        switch (subclass_code)
        {
        case 0x06:
            switch (prog_IF)
            {
            case 0x00:
                return "Vendor Specific Interface";
            case 0x01:
                return "AHCI 1.0";
            case 0x02:
                return "Serial Storage Bus";
            }
        }
    case 0x03:
        switch (subclass_code)
        {
        case 0x00:
            switch (prog_IF)
            {
            case 0x00:
                return "VGA Controller";
            case 0x01:
                return "8514-Compatible Controller";
            }
        }
    case 0x0C:
        switch (subclass_code)
        {
        case 0x03:
            switch (prog_IF)
            {
            case 0x00:
                return "UHCI Controller";
            case 0x10:
                return "OHCI Controller";
            case 0x20:
                return "EHCI (USB2) Controller";
            case 0x30:
                return "XHCI (USB3) Controller";
            case 0x80:
                return "Unspecified";
            case 0xFE:
                return "USB Device (Not a Host Controller)";
            }
        }
    }
    return NULL;
}
