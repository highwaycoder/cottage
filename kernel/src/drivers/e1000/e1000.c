#include <drivers/e1000/e1000.h>
#include <pci/pci.h>
#include <acpi/acpi.h>
#include <mem/pmm.h>
#include <klog/klog.h>
#include <panic.h>
#include <stdbool.h>
#include <net/network.h>
#include <errors/errno.h>
#include <string.h>
#include <stdlib.h>
#include <interrupt/apic.h>
#include <scheduler/semaphore.h>

network_device_t dev;

// RX/TX ring pointers
uint8_t *rx_ptr;
uint8_t *tx_ptr;

// RX/TX indices
uint16_t rx_cur;
uint16_t tx_cur;

// RX/TX descriptor buffers
e1000_rx_desc* rx_descs[E1000_NUM_RX_DESC];
e1000_tx_desc* tx_descs[E1000_NUM_TX_DESC];

// mac address
uint8_t mac_address[6];

// driver internals
static uint64_t g_mmio_address;
static bool eeprom_exists;
static uint32_t ip;

// "internal" functions (not exposed via header)
static void read_mac_address();
static uint32_t read_command(uint16_t offset);
static void write_command(uint16_t address, uint32_t value);
static bool detect_eeprom();
static void start_link();
static void rx_init();
static void tx_init();

// todo: support multiple IP addresses?
void set_ip(uint32_t new_ip)
{
    ip = new_ip;
}

uint32_t get_ip()
{
    return ip;
}

bool detect_eeprom()
{
    write_command(REG_EEPROM, 0x1);

    for(uint16_t i = 0; i < 1000; i++)
    {
        if(read_command(REG_EEPROM) & 0x10) return true;
    }

    return false;
}

void start_link()
{
    uint32_t val = read_command(REG_CTRL);
    write_command(REG_CTRL, val | ECTRL_SLU);
}

void rx_init()
{
    e1000_rx_desc* descs = (e1000_rx_desc*) rx_ptr;
    for(uint16_t i = 0; i < E1000_NUM_RX_DESC; i++)
    {
        rx_descs[i] = (e1000_rx_desc*) ((uint8_t*) descs + i * 16 + HIGHER_HALF);
        rx_descs[i]->addr = (uint64_t)(uint8_t *) pmm_alloc(((8192 + 16) / 0x1000) + 1);
        rx_descs[i]->status = 0;
    }

    write_command(REG_RXDESCLO, (uint64_t) rx_ptr);
    write_command(REG_RXDESCHI, 0);

    write_command(REG_RXDESCLEN, E1000_NUM_RX_DESC * 16);

    write_command(REG_RXDESCHEAD, 0);
    write_command(REG_RXDESCTAIL, E1000_NUM_RX_DESC - 1);
    rx_cur = 0;

    write_command(REG_RCTRL, RCTL_EN| RCTL_SBP| RCTL_UPE | RCTL_MPE | RCTL_LBM_NONE | RTCL_RDMTS_HALF | RCTL_BAM | RCTL_SECRC  | RCTL_BSIZE_8192);
}

void tx_init()
{
    e1000_tx_desc* descs = (e1000_tx_desc*) tx_ptr;
    for(uint16_t i = 0; i < E1000_NUM_TX_DESC; i++)
    {
        tx_descs[i] = (e1000_tx_desc*) ((uint8_t*)descs + i * 16 + HIGHER_HALF);
        // allocate one page for tx buffer
        tx_descs[i]->addr = (uint64_t)pmm_alloc(1);
        tx_descs[i]->cmd = 0;
        tx_descs[i]->status = TSTA_DD;
    }

    write_command(REG_TXDESCLO, (uint64_t) tx_ptr);
	write_command(REG_TXDESCHI, 0);

	write_command(REG_TXDESCLEN, E1000_NUM_TX_DESC * 16);

	write_command(REG_TXDESCHEAD, 0);
	write_command(REG_TXDESCTAIL, 0);
	tx_cur = 0;

	write_command(REG_TCTRL, 0b0110000000000111111000011111010);
	write_command(REG_TIPG, 0x0060200A);

}

void set_mac_address(uint8_t new_addr[6])
{
    uint32_t mac_low = 0;
    uint32_t mac_high = 0;
    mac_low += new_addr[0];
    mac_low += new_addr[1] << 8;
    mac_low += new_addr[2] << 16;
    mac_low += new_addr[3] << 24;
    mac_high += new_addr[4];
    mac_high += new_addr[5] << 8;

    write_command(0x5400, mac_low);
    write_command(0x5404, mac_high);
}

ssize_t e1000_send(uint8_t* data, uint16_t len)
{
    if (!(tx_descs[tx_cur]->status & TSTA_DD)) {
        return -ENOBUFS;
    }
    uint8_t* virt_buf = (uint8_t *)tx_descs[tx_cur]->addr + HIGHER_HALF;
    memcpy(virt_buf, data, len);
    tx_descs[tx_cur]->length = len;
    
    tx_descs[tx_cur]->cmd = CMD_EOP | CMD_IFCS | CMD_RS;
    tx_descs[tx_cur]->status = 0;
    tx_cur = (tx_cur + 1) % E1000_NUM_TX_DESC;
    write_command(REG_TXDESCTAIL, tx_cur);

    return len;
}

void e1000_init(uint64_t mmio_address, uint16_t bus, uint16_t device, uint16_t function)
{
    eeprom_exists = false;
    rx_ptr = pmm_alloc(((sizeof(e1000_rx_desc) * E1000_NUM_RX_DESC + 16) / 0x1000) + 1);
    tx_ptr = pmm_alloc(((sizeof(e1000_tx_desc) * E1000_NUM_TX_DESC + 16) / 0x1000) + 1);

    g_mmio_address = mmio_address + HIGHER_HALF;

    eeprom_exists = detect_eeprom();
    read_mac_address();

    klog("e1000", "MAC Address: %x:%x:%x:%x:%x:%x",
         mac_address[0],
         mac_address[1],
         mac_address[2],
         mac_address[3],
         mac_address[4],
         mac_address[5]);

    start_link();
    for(int i = 0; i < 0x80; i++)
    {
        write_command(0x5200 + i * 4, 0);
    }

    // enable interrupts
    write_command(REG_IMASK, 0x1F6DC);
    write_command(REG_IMASK, 0xff & ~4);
    read_command(0xc0);

    rx_init();
    tx_init();

    klog("e1000", "Activated ethernet card");
    
    set_ip(0);

    // set up the abstract network device 
    // and register it with the abstraction layer
    memcpy(&dev, &((network_device_t) {
        .name = "e1000",
        .transmit = e1000_send,
        .flags = NET_DEV_STATUS_ENABLE | NET_DEV_STATUS_LINK | NET_DEV_STATUS_LINK_READY,
        .ip4 = {10, 0, 2, 15},
    }), sizeof(network_device_t));
    memcpy(&dev.mac, mac_address, 6);

    // initialize the recv_queue structure (necessary)
    packet_queue_init(&dev.recv_queue, 64);

    // set up MSI
    uint8_t vec = idt_allocate_vector();
    interrupt_table[vec] = (void*)e1000_interrupt_handler;
    pci_enable_msi(bus, device, function, vec, 0);

    // Re-enable interrupts after MSI setup - clear any pending first, then set mask
    read_command(0xC0);  // Clear pending interrupts by reading ICR
    write_command(REG_IMASK, 0x1F6DC);  // Enable RX and other interrupts

    net_register_device("eth0", &dev);
}

void e1000_interrupt_handler(uint32_t num, cpu_status_t* status)
{
    // NOTE: Cannot call klog() from ISR - would deadlock if interrupted code holds klog_lock
    read_command(0xC0); // ICR read clears the interrupt
    uint16_t new_tail = rx_cur;
    while (rx_descs[rx_cur]->status & 1)
    {
        uint8_t* packet_data = (uint8_t*)(rx_descs[rx_cur]->addr + HIGHER_HALF);
        uint16_t packet_len = rx_descs[rx_cur]->length;

        uint32_t tail = atomic_load(&dev.recv_queue.tail);
        uint32_t next_tail = (tail+1) % dev.recv_queue.slot_count;

        if(next_tail != atomic_load(&dev.recv_queue.head)) {
            // copy to slot
            uint8_t* slot = dev.recv_queue.data + (tail * dev.recv_queue.slot_size);
            memcpy(slot, packet_data, packet_len);

            // set metadata
            dev.recv_queue.meta[tail].length = packet_len;
            dev.recv_queue.meta[tail].flags = 0;

            // Advance tail
            atomic_store(&dev.recv_queue.tail, next_tail);

            sem_signal(&dev.recv_queue.packet_ready);
        } else {
            // Packet dropped: queue full (can't log from ISR)
        }

        rx_descs[rx_cur]->status = 0;
        rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;
    }

    write_command(REG_RXDESCTAIL, new_tail);
    lapic_eoi();
}

void write_command(uint16_t address, uint32_t value)
{
    *((uint32_t *)(g_mmio_address + address)) = value;
}

uint32_t read_command(uint16_t offset)
{
    return *((uint32_t *)(g_mmio_address + offset));
}

void read_mac_address()
{
    uint32_t mac_low = read_command(0x5400);
    uint32_t mac_high = read_command(0x5404);
    mac_address[0] = mac_low & 0xff;
    mac_address[1] = mac_low >> 8 & 0xff;
    mac_address[2] = mac_low >> 16 & 0xff;
    mac_address[3] = mac_low >> 24 & 0xff;

    mac_address[4] = mac_high & 0xff;
    mac_address[5] = mac_high >> 8 & 0xff;
}
