#pragma once

#include <acpispec/tables.h>
#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>

typedef enum {
    PCI_BAR_TYPE_NONE,
    PCI_BAR_TYPE_MMIO64,
    PCI_BAR_TYPE_MMIO32,
    PCI_BAR_TYPE_IO
} pci_bar_type_t;

typedef struct {
    pci_bar_type_t type;
    uint64_t mem_address;
    uint16_t io_address;
    size_t size;
} pci_bar_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
} __attribute__((packed)) pci_device_header_t;

typedef struct {
		pci_device_header_t header;
		uint32_t BAR0;
		uint32_t BAR1;
		uint32_t BAR2;
		uint32_t BAR3;
		uint32_t BAR4;
		uint32_t BAR5;
		uint32_t cardbus_CIS_ptr;
		uint16_t subsystem_vendor_ID;
		uint16_t subsystem_ID;
		uint32_t expansion_ROM_base_addr;
		uint8_t capabilities_ptr;
		uint8_t rsv0;
		uint16_t rsv1;
		uint32_t rsv2;
		uint8_t interrupt_line;
		uint8_t interrupt_pin;
		uint8_t min_grant;
		uint8_t max_latency;
} __attribute__((packed)) pci_header_0_t;

typedef struct {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t start_pci_bus;
    uint8_t end_pci_bus;
    uint32_t _resv0;            // 4 bytes reserved
} __attribute__((packed)) mcfg_device_config_t;

typedef struct {
    acpi_header_t header;
    uint64_t _resv0;            // 8 bytes reserved
} __attribute__((packed)) mcfg_header_t;

// read/write to PCI
uint8_t pci_readb(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset);
uint16_t pci_readw(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset);
uint32_t pci_readd(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset);
void pci_writeb(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset, uint8_t val);
void pci_writew(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset, uint16_t val);
void pci_writed(uint16_t bus, uint16_t device, uint16_t function, uint32_t registeroffset, uint32_t val);

// pci enumeration stuff
void pci_init();
const char* get_vendor_name(uint16_t vendor_ID);
const char* get_device_name(uint16_t vendor_ID, uint16_t device_ID);
const char* get_subclass_name(uint8_t class_code, uint8_t subclass_code);
const char* get_prog_IF_name(uint8_t class_code, uint8_t subclass_code, uint8_t prog_IF);

// BAR register helper functions
pci_bar_t pci_get_bar(uint32_t* bar0, int bar_num, uint16_t bus, uint16_t device, uint16_t function);

// I/O helper functions
void pci_enable_mmio(uint16_t bus, uint16_t device, uint16_t function);
void pci_become_bus_master(uint16_t bus, uint16_t device, uint16_t function);

// capability helper functions
uint8_t pci_find_capability(uint16_t bus, uint16_t dev, uint16_t func, uint8_t cap_id);

// MSI setup
ssize_t pci_enable_msi(uint16_t bus, uint16_t dev, uint16_t func, uint8_t vector, uint8_t lapic_id);
