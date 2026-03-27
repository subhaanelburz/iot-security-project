// Socket Library
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
#include "arp.h"
#include "ip.h"
#include "udp.h"
#include "tcp.h"

// max number of sockets at the same time
#define MAX_SOCKETS 10

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

uint8_t socketCount = 0;        // number of active sockets? not used
socket sockets[MAX_SOCKETS];    // create array of 10 sockets we can use

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void initSockets(void)
{
    uint8_t i;
    for (i = 0; i < MAX_SOCKETS; i++)
        sockets[i].state = TCP_CLOSED;  // set all 10 sockets to default state
}

// this will find the first free slot in the socket array
// very similar to the code in timer.c
socket * newSocket(void)
{
    uint8_t i = 0;
    socket * s = NULL;
    bool foundUnused = false;
    while (i < MAX_SOCKETS && !foundUnused) // iterate through array and look for free slot
    {
        foundUnused = sockets[i].state == TCP_CLOSED;   // check the state of socket
        if (foundUnused)
            s = &sockets[i];    // if there is a free slot, return pointer here
        i++;
    }
    return s;
}

// this will free up a socket slot in the array works
// by finding it and change it back to default state
void deleteSocket(socket *s)
{
    uint8_t i = 0;
    bool foundMatch = false;
    while (i < MAX_SOCKETS && !foundMatch)
    {
        foundMatch = &sockets[i] == s;  // check if it is the socket we are looking for
        if (foundMatch)
            sockets[i].state = TCP_CLOSED;  // if it is, set the state back to default
        i++;
    }
}

// Get socket information from a received ARP response message
// when we get an arp response, we will just fill in the socket
void getSocketInfoFromArpResponse(etherHeader *ether, socket *s)
{
    arpPacket *arp = (arpPacket*)ether->data;
    uint8_t i;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        s->remoteHwAddress[i] = arp->sourceAddress[i];  // set the mac address
    for (i = 0; i < IP_ADD_LENGTH; i++)
        s->remoteIpAddress[i] = arp->sourceIp[i];       // set the ip address
}

// Get socket information from a received UDP packet
void getSocketInfoFromUdpPacket(etherHeader *ether, socket *s)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    uint8_t i;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        s->remoteHwAddress[i] = ether->sourceAddress[i];    // get mac address from ethernet layer of UDP packet
    for (i = 0; i < IP_ADD_LENGTH; i++)
        s->remoteIpAddress[i] = ip->sourceIp[i];            // get ip address from UDP packet
    s->remotePort = ntohs(udp->sourcePort);                 // get their port from UDP layer
    s->localPort = ntohs(udp->destPort);                    // get our own port
}

// Get socket information from a received TCP packet
// same thing as UDP packet but now gets from TCP packet
void getSocketInfoFromTcpPacket(etherHeader *ether, socket *s)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    tcpHeader* tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    uint8_t i;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        s->remoteHwAddress[i] = ether->sourceAddress[i];
    for (i = 0; i < IP_ADD_LENGTH; i++)
        s->remoteIpAddress[i] = ip->sourceIp[i];
    s->remotePort = ntohs(tcp->sourcePort);
    s->localPort = ntohs(tcp->destPort);
}
