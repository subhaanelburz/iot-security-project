// ARP Library
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: -
// Target uC:       -
// System Clock:    -

// Hardware configuration:
// -

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#ifndef ARP_H_
#define ARP_H_

#include <stdint.h>
#include <stdbool.h>
#include "eth0.h"
#include "ip.h"

// arp = address resolution protocol
// it basically maps all of the ip addresses to the physical mac addresses
// request: "who has 192.168.1.2?"
// response: "192.168.1.2 is 0A:24:23:59 (some mac address)"
// it is directly after etherHeader; it is not apart of IP
typedef struct _arpPacket // 28 bytes
{
  uint16_t hardwareType;    // 1 = ethernet
  uint16_t protocolType;    // 0x800 = ipv4
  uint8_t hardwareSize;     // mac address is 6 bytes long
  uint8_t protocolSize;     // ipv4 address is 4 bytes long
  uint16_t op;              // 1 = request, 2 = reply
  uint8_t sourceAddress[6]; // sender mac address
  uint8_t sourceIp[4];      // sender ip address
  uint8_t destAddress[6];   // destination mac address (this is set as all 1s/Fs for requests)
  uint8_t destIp[4];        // destination ip address (who we are trying to find)
} arpPacket;

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

bool isArpRequest(etherHeader *ether);
bool isArpResponse(etherHeader *ether);
void sendArpResponse(etherHeader *ether);
void sendArpRequest(etherHeader *ether, uint8_t ipFrom[], uint8_t ipTo[]);

#endif

