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

#include <stdio.h>
#include "arp.h"
#include "ip.h"

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// Determines whether packet is ARP request
bool isArpRequest(etherHeader *ether)
{
    arpPacket *arp = (arpPacket*)ether->data;   // extract the arp packet from ethernet data
    bool ok;
    uint8_t i = 0;
    uint8_t localIpAddress[IP_ADD_LENGTH];
    ok = (ether->frameType == htons(TYPE_ARP)); // check if it is an ARP packet
    getIpAddress(localIpAddress);               // get current IP address
    while (ok && (i < IP_ADD_LENGTH))
    {
        ok = (arp->destIp[i] == localIpAddress[i]); // check if the the arp packet was meant to be sent to the tm4c ip address
        i++;
    }
    if (ok)
        ok = (arp->op == htons(1)); // then we check if it is a request; 1 = request
    return ok;
}

// Determines whether packet is ARP response
// similar to isArpRequest but it doesnt check
// if the packet was meant to be sent to the tm4c
bool isArpResponse(etherHeader *ether)
{
    arpPacket *arp = (arpPacket*)ether->data;
    bool ok;
    ok = (ether->frameType == htons(TYPE_ARP)); // check if the packet is ARP
    if (ok)
        ok = (arp->op == htons(2)); // check if it is a response; 2 = response
    return ok;
}

// Sends an ARP response given the request data
// basically we use this when some other device
// is requesting our MAC address, we send an ARP response
// to tell them that my ip address is at this MAC address
void sendArpResponse(etherHeader *ether)
{
    arpPacket *arp = (arpPacket*)ether->data;
    uint8_t i, tmp;
    uint8_t localHwAddress[HW_ADD_LENGTH];

    // set op to response (response = 2)
    arp->op = htons(2);
    // swap source and destination fields
    getEtherMacAddress(localHwAddress);
    for (i = 0; i < HW_ADD_LENGTH; i++) // swap mac address (ethernet layer)
    {
        arp->destAddress[i] = arp->sourceAddress[i];
        ether->destAddress[i] = ether->sourceAddress[i];
        ether->sourceAddress[i] = arp->sourceAddress[i] = localHwAddress[i];    // here we put our mac address as the source
    }
    for (i = 0; i < IP_ADD_LENGTH; i++) // swap ip addresses (arp layer)
    {
        tmp = arp->destIp[i];
        arp->destIp[i] = arp->sourceIp[i];
        arp->sourceIp[i] = tmp;
    }
    // send packet
    putEtherPacket(ether, sizeof(etherHeader) + sizeof(arpPacket));
}

// Sends an ARP request
// this is when we are asking everyone (broadcasting)
// if you have this ip, tell me your mac address
// so in the packet we send out, we have the target ip address
// then send to everyone, whoever doesnt have the ip ignores it
// that is why the isArpRequest checks if the ip address is meant for them
// whoever the ip belongs to, will then respond with their mac address
void sendArpRequest(etherHeader *ether, uint8_t ipFrom[], uint8_t ipTo[])
{
    arpPacket *arp = (arpPacket*)ether->data;
    uint8_t i;
    uint8_t localHwAddress[HW_ADD_LENGTH];

    // fill ethernet frame
    getEtherMacAddress(localHwAddress);
    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        ether->sourceAddress[i] = localHwAddress[i];    // here we put our mac address as the source
        ether->destAddress[i] = 0xFF;                   // broadcast to everyone (all Fs)
    }
    ether->frameType = htons(TYPE_ARP); // declare ethernet packet to be ARP
    // fill arp frame
    arp->hardwareType = htons(1);       // set to ethernet (1)
    arp->protocolType = htons(TYPE_IP); // set to ipv4 (0x800)
    arp->hardwareSize = HW_ADD_LENGTH;  // mac address = 6 bytes
    arp->protocolSize = IP_ADD_LENGTH;  // ip address = 4 bytes
    arp->op = htons(1);                 // set arp packet to 1 (request)
    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        arp->sourceAddress[i] = localHwAddress[i];  // fill out same info but in arp packet
        arp->destAddress[i] = 0xFF;
    }
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        arp->sourceIp[i] = ipFrom[i];   // fill out our ip address
        arp->destIp[i] = ipTo[i];       // fill the ip address of who we are trying to find
    }
    // send packet
    putEtherPacket(ether, sizeof(etherHeader) + sizeof(arpPacket));
}
