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

#include <stdio.h>
#include "udp.h"

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// Determines whether packet is UDP datagram
// Must be an IP packet
bool isUdp(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);   // get the udp header
    bool ok;
    uint16_t tmp16;
    uint32_t sum = 0;
    ok = (ip->protocol == PROTOCOL_UDP);    // check if ip protocol is 17 (UDP)
    if (ok)
    {
        // 32-bit sum over pseudo-header
        // for UDP, you have to do a checksum with a pseudo-header
        // which is different since it gives protection against
        // misrouted datagrams (from rfc 768)
        sumIpWords(ip->sourceIp, 8, &sum);  // sum up source ip and destination ip
        tmp16 = ip->protocol;
        sum += (tmp16 & 0xff) << 8;         // must add the protocol byte into upper byte of sum
        sumIpWords(&udp->length, 2, &sum);  // add the last udp length field for pseudoheader
        // add udp header and data
        sumIpWords(udp, ntohs(udp->length), &sum);  // now we do the actual summation of UDP header and data
        ok = (getIpChecksum(sum) == 0);             // finish checksum by inverting and ensure it = 0
    }
    return ok;
}

// Gets pointer to UDP payload of frame
// literally just return pointer to start of UDP data
// where the dhcp messages actually are
uint8_t * getUdpData(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    return udp->data;
}

// Send UDP message
// this function creates the entire UDP packet we will send out
void sendUdpMessage(etherHeader *ether, socket s, uint8_t data[], uint16_t dataSize)
{
    uint8_t i;
    uint16_t j;
    uint32_t sum;
    uint16_t tmp16;
    uint16_t udpLength;
    uint8_t *copyData;
    uint8_t localHwAddress[6];
    uint8_t localIpAddress[4];

    // Ether frame
    getEtherMacAddress(localHwAddress);     // getting our local mac address
    getIpAddress(localIpAddress);           // getting our local ip address
    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        ether->destAddress[i] = s.remoteHwAddress[i];   // socket has mac address of who we are talking to
        ether->sourceAddress[i] = localHwAddress[i];    // our mac address we got from calling function
    }
    ether->frameType = htons(TYPE_IP);  // set the ethernet frame type for an IP packet

    // IP header
    ipHeader* ip = (ipHeader*)ether->data;
    ip->rev = 0x4;              // set as ipv4 datagram
    ip->size = 0x5;             // the size is 4*5 = 20 bytes with no options
    ip->typeOfService = 0;
    ip->id = 0;
    ip->flagsAndOffset = 0;
    ip->ttl = 128;                  // set time to live to 128 jumps
    ip->protocol = PROTOCOL_UDP;    // set ip protocol to UDP
    ip->headerChecksum = 0;
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        ip->destIp[i] = s.remoteIpAddress[i];   // get destination ip from socket
        ip->sourceIp[i] = localIpAddress[i];    // get local ip address
    }
    uint8_t ipHeaderLength = ip->size * 4;

    // UDP header
    udpHeader* udp = (udpHeader*)((uint8_t*)ip + (ip->size * 4));
    udp->sourcePort = htons(s.localPort);           // get the local UDP port from socket
    udp->destPort = htons(s.remotePort);            // get the destination UDP port from socket
    // adjust lengths
    udpLength = sizeof(udpHeader) + dataSize;       // set UDP length to header + payload size
    ip->length = htons(ipHeaderLength + udpLength); // set total ip length to ip header + UDP length
    // 32-bit sum over ip header
    calcIpChecksum(ip);                 // calculate the full IP checksum
    // set udp length
    udp->length = htons(udpLength);
    // copy data
    copyData = udp->data;               // create pointer to udp data location
    for (j = 0; j < dataSize; j++)
        copyData[j] = data[j];          // now copy the data into the actual packet
    // 32-bit sum over pseudo-header
    sum = 0;
    sumIpWords(ip->sourceIp, 8, &sum);
    tmp16 = ip->protocol;
    sum += (tmp16 & 0xff) << 8;
    sumIpWords(&udp->length, 2, &sum);
    // add udp header
    udp->check = 0;
    sumIpWords(udp, udpLength, &sum);
    udp->check = getIpChecksum(sum);    // finish UDP checksum; same as in isUdp function

    // send packet with size = ether + udp hdr + ip header + udp_size
    putEtherPacket(ether, sizeof(etherHeader) + ipHeaderLength + udpLength);
}
