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

#include <stdio.h>
#include "icmp.h"

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------


// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// Determines whether packet is ping request
// Must be an IP packet
bool isPingRequest(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;  // get ip header
    uint8_t ipHeaderLength = ip->size * 4;
    icmpHeader *icmp = (icmpHeader*)((uint8_t*)ip + ipHeaderLength);    // get icmp header
    return (ip->protocol == PROTOCOL_ICMP && icmp->type == 8);          // check if it is ICMP, and if it is a ping request (=8)
}

// Sends a ping request
// prof removed it, he said he probably just forgot to add; not needed
void sendPingRequest(etherHeader *ether, uint8_t ipAdd[])
{
    // move code here from master icmp lib
}

// Sends a ping response given the request data
void sendPingResponse(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    icmpHeader *icmp = (icmpHeader*)((uint8_t*)ip + ipHeaderLength); // note: ip->data not used b/c could have options that mess everything up
    uint8_t i, tmp;
    uint16_t icmp_size;
    uint32_t sum = 0;
    // swap source and destination fields
    for (i = 0; i < HW_ADD_LENGTH; i++) // swap the mac addresses to send the ping response (ethernet layer)
    {
        tmp = ether->destAddress[i];
        ether->destAddress[i] = ether->sourceAddress[i];
        ether->sourceAddress[i] = tmp;
    }
    for (i = 0; i < IP_ADD_LENGTH; i++) // swap the ip addresses to send the ping response (ip layer)
    {
        tmp = ip->destIp[i];
        ip->destIp[i] = ip ->sourceIp[i];
        ip->sourceIp[i] = tmp;
    }
    // this is a response
    icmp->type = 0;             // change from 8 (request) to 0 (response)
    // calc icmp checksum
    icmp->check = 0;            // set checksum field to 0
    icmp_size = ntohs(ip->length) - ipHeaderLength; // get size of icmp packet; total ip length - ip header length = icmp length
    sumIpWords(icmp, icmp_size, &sum);  // sum up all icmp bytes
    icmp->check = getIpChecksum(sum);   // do the carries and inverting
    // send packet
    putEtherPacket(ether, sizeof(etherHeader) + ntohs(ip->length)); // lastly send the actual packet
}
