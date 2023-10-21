#pragma once

#include <acpispec/tables.h>
#include <stdint.h>

typedef struct {
    acpi_header_t header;
    uint8_t hardware_rev_id;
    uint8_t info;
    uint16_t pci_id;
    uint8_t address_space_id;
    uint8_t register_width;
    uint8_t register_offset;
    uint8_t reserved;
    uint64_t address;
    uint8_t hpet_num;
    uint16_t minim_ticks;
    uint8_t page_protection;
} __attribute__((packed)) hpet_table_t;

typedef struct {
		uint64_t capabilities;
		uint64_t unused0;
		uint64_t general_config;
		uint64_t unused1;
		uint64_t int_status;
		uint64_t unused2;
		uint64_t unused3[24];
		uint64_t counter_value;
		uint64_t unused4;
} __attribute__((packed)) hpet_t;

void init_timer();
void sleep(uint32_t millis);
uint64_t get_ticks_per_second();
uint64_t get_ticks();