#pragma once

#include <stdint.h>

// NOTE: some QEMU versions reportedly use device 0x100E
// rather than 10D3 - they are similar devices, perhaps even compatible,
// but I've written the driver against my own QEMU version and cannot guarantee
// that it will work for others.  Modify DEVICE to 0x100E at your own risk.
#define E1000_PCI_VENDOR 0x8086
#define E1000_PCI_DEVICE 0x10D3
#define E1000_PCI_FUNCTN 0x0000

// the E1000 has 32 RX descriptors, and 8 TX descriptors
#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8

// general purpose registers
#define REG_CTRL		0x0000
#define REG_STATUS		0x0008
#define REG_EEPROM		0x0014
#define REG_CTRL_EXT	0x0018
#define REG_IMASK		0x00D0

// RX registers
#define REG_RCTRL		0x0100
#define REG_RXDESCLO	0x2800
#define REG_RXDESCHI	0x2804
#define REG_RXDESCLEN	0x2808
#define REG_RXDESCHEAD	0x2810
#define REG_RXDESCTAIL	0x2818

// TX registers
#define REG_TCTRL		0x0400
#define REG_TXDESCLO	0x3800
#define REG_TXDESCHI	0x3804
#define REG_TXDESCLEN	0x3808
#define REG_TXDESCHEAD	0x3810
#define REG_TXDESCTAIL	0x3818

#define RCTL_EN							(1 << 1) //Receiver Enable
#define RCTL_SBP						(1 << 2) //Store Bad Packets
#define RCTL_UPE						(1 << 3) //Unicast Promiscuous Enabled
#define RCTL_MPE						(1 << 4) //Multicast Promiscuous Enabled
#define RCTL_LPE						(1 << 5) //Long Packet Reception Enable
#define RCTL_LBM_NONE					(0 << 6) //No Loopback
#define RCTL_LBM_PHY					(3 << 6) //PHY or external SerDesc loopback
#define RTCL_RDMTS_HALF					(0 << 8) //Free Buffer Threshold is 1/2 of RDLEN
#define RTCL_RDMTS_QUARTER				(1 << 8) //Free Buffer Threshold is 1/4 of RDLEN
#define RTCL_RDMTS_EIGHTH				(2 << 8) //Free Buffer Threshold is 1/8 of RDLEN
#define RCTL_MO_36						(0 << 12) //Multicast Offset - bits 47:36
#define RCTL_MO_35						(1 << 12) //Multicast Offset - bits 46:35
#define RCTL_MO_34						(2 << 12) //Multicast Offset - bits 45:34
#define RCTL_MO_32						(3 << 12) //Multicast Offset - bits 43:32
#define RCTL_BAM						(1 << 15) //Broadcast Accept Mode
#define RCTL_VFE						(1 << 18) //VLAN Filter Enable
#define RCTL_CFIEN						(1 << 19) //Canonical Form Indicator Enable
#define RCTL_CFI						(1 << 20) //Canonical Form Indicator Bit Value
#define RCTL_DPF						(1 << 22) //Discard Pause Frames
#define RCTL_PMCF						(1 << 23) //Pass MAC Control Frames
#define RCTL_SECRC						(1 << 26) //Strip Ethernet CRC

// Buffer Sizes
#define RCTL_BSIZE_256					(3 << 16)
#define RCTL_BSIZE_512					(2 << 16)
#define RCTL_BSIZE_1024					(1 << 16)
#define RCTL_BSIZE_2048					(0 << 16)
#define RCTL_BSIZE_4096					((3 << 16) | (1 << 25))
#define RCTL_BSIZE_8192					((2 << 16) | (1 << 25))
#define RCTL_BSIZE_16384				((1 << 16) | (1 << 25))

// idk what a transmit inter packet gap is either
#define REG_TIPG		0x0410 //Transmit Inter Packet Gap
#define ECTRL_SLU		0x40 //Set link up

// STA flags
#define TSTA_DD							(1 << 0) //Descriptor Done
#define TSTA_EC							(1 << 1) //Excess Collisions
#define TSTA_LC							(1 << 2) //Late Collision
#define LSTA_TU							(1 << 3) //Transmit Underrun
                                                 
// CMD flags
#define CMD_EOP                         (1 << 0) // End of Packet
#define CMD_IFCS                        (1 << 1) // Insert FCS/CRC
#define CMD_RS                          (1 << 3) // Report Status

// abstraction layer settings
// the number of pages to allocate for the in-kernel send buffer,
// this is not the same as the TX buffer, which is managed by the NIC itself
#define RECV_BUF_PAGES 32

typedef struct {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint16_t checksum;
    volatile uint8_t status;
    volatile uint8_t errors;
    volatile uint16_t special;
} __attribute__((packed)) e1000_rx_desc;

typedef struct {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint8_t cso;
    volatile uint8_t cmd;
    volatile uint8_t status;
    volatile uint8_t css;
    volatile uint16_t special;
} __attribute__((packed)) e1000_tx_desc;

void e1000_init(uint64_t mmio_address);
void set_ip(uint32_t new_ip);
uint32_t get_ip();
