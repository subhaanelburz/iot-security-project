// UDP Library
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

#ifndef UDP_H_
#define UDP_H_

#include <stdint.h>
#include <stdbool.h>
#include "ip.h"
#include "socket.h"

// udp = user datagram protocol
// in memory:   etherHeader->data = ipHeader
//              ipHeader->data = udpHeader (IF NO OPTIONS!!)
// udp is a transport layer protocol, and basically
// it transports UDP datagrams, the actual data
// UDP create datagrams; IP layer makes it into a packet then
// packet is turned into ethernet frame to move across internet
typedef struct _udpHeader // 8 bytes
{
  uint16_t sourcePort;  // the port we are sending from; dhcp client = 68
  uint16_t destPort;    // the port we are sending to; dhcp server = 67
  uint16_t length;      // total length of UDP header and data
  uint16_t check;       // checksum for UDP header and data
  uint8_t  data[0];     // here is the actual data payload
} udpHeader;

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

bool isUdp(etherHeader *ether);
uint8_t* getUdpData(etherHeader *ether);
void getUdpMessageSocket(etherHeader *ether, socket *s);
void sendUdpMessage(etherHeader *ether, socket s, uint8_t data[], uint16_t dataSize);

#endif
