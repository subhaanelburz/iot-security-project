// DHCP Library
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: EK-TM4C123GXL w/ ENC28J60
// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

// Hardware configuration:
// ENC28J60 Ethernet controller on SPI0
//   MOSI (SSI0Tx) on PA5
//   MISO (SSI0Rx) on PA4
//   SCLK (SSI0Clk) on PA2
//   ~CS (SW controlled) on PA3
//   WOL on PB3
//   INT on PC6

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

// Name: Subhaan Elburz
// ID:   1002135522

#ifndef DHCP_H_
#define DHCP_H_

#include <stdint.h>
#include <stdbool.h>
#include "udp.h"

// this is the dhcp structure format from rfc 2131 on page 8
// dhcp is on top of UDP, the format in memory is:
// etherHeader->data = ipHeader
// ipHeader->data = udpHeader (IF NO OPTIONS!!)
// udpHeader->data = DHCP frame
typedef struct _dhcpFrame // 240 or more bytes
{
  uint8_t op;           // 1 = request / client to server, 2 = reply / server to client
  uint8_t htype;        // hardware type, 1 = ethernet
  uint8_t hlen;         // hardware address length (mac address) = 6 bytes
  uint8_t hops;         // rfc2131 says client just sets this to 0; relay agents?
  uint32_t  xid;        // random transaction ID so we can match the server's reply
  uint16_t secs;        // seconds elapsed since client began dhcp
  uint16_t flags;       // broadcast flag
  uint8_t ciaddr[4];    // client ip address
  uint8_t yiaddr[4];    // the assigned ip address by server, your ip
  uint8_t siaddr[4];    // the server ip address
  uint8_t giaddr[4];    // relay agent ip address
  uint8_t chaddr[16];   // this is the client hardware address
  uint8_t data[192];    // zeroed data that is unused because part of older bootP
  uint32_t magicCookie; // magic cookie basically says this is a dhcp packet
  uint8_t options[0];   // array for dhcp options
} dhcpFrame;

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

bool isDhcpResponse(etherHeader *ether);

void sendDhcpPendingMessages(etherHeader *ether);
void processDhcpResponse(etherHeader *ether);
void processDhcpArpResponse(etherHeader *ether);

void enableDhcp(void);
void disableDhcp(void);
bool isDhcpEnabled(void);

void renewDhcp(void);
void releaseDhcp(void);

uint32_t getDhcpLeaseSeconds();

#endif
