// ICMP Library
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

#ifndef ICMP_H_
#define ICMP_H_

#include <stdint.h>
#include <stdbool.h>
#include "ip.h"

// icmp = internet control message protocol
// basically for pinging/troubleshooting
// in memory:   etherHeader->data = ipHeader
//              ipHeader->data = icmpHeader (IF NO OPTIONS!!)
typedef struct _icmpHeader // 8 bytes
{
  uint8_t type; // 8 = ping/echo request; 0 = ping/echo response
  uint8_t code;
  uint16_t check;   // checksum for entire icmp packet
  uint16_t id;      // packet id
  uint16_t seq_no;  // sequence number
  uint8_t data[0];
} icmpHeader;

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

bool isPingRequest(etherHeader *ether);
void sendPingRequest(etherHeader *ether, uint8_t ipAdd[]);
void sendPingResponse(etherHeader *ether);

#endif
