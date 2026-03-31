// DHCP Library
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
#include <stdlib.h>
#include "dhcp.h"
#include "arp.h"
#include "timer.h"
#include "uart0.h"

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7
#define DHCPINFORM   8

#define DHCP_DISABLED   0
#define DHCP_INIT       1
#define DHCP_SELECTING  2
#define DHCP_REQUESTING 3
#define DHCP_TESTING_IP 4
#define DHCP_BOUND      5
#define DHCP_RENEWING   6
#define DHCP_REBINDING  7
#define DHCP_INITREBOOT 8 // not used since ip not stored over reboot
#define DHCP_REBOOTING  9 // not used since ip not stored over reboot

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

uint32_t xid = 0;
uint32_t leaseSeconds = 0;
uint32_t leaseT1 = 0;
uint32_t leaseT2 = 0;
uint8_t gatewayIp[4];

// use these variables if you want
bool discoverNeeded = false;
bool requestNeeded = false;
bool releaseNeeded = false;

bool ipConflictDetectionMode = false;

uint8_t dhcpOfferedIpAdd[4];
uint8_t dhcpServerIpAdd[4];

uint8_t dhcpState = DHCP_DISABLED;
bool    dhcpEnabled = true;

// this is for testing nak
bool forceNakTest = false;

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------
typedef struct _dhcpPacket
{
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magicCookie;
} dhcpPacket;
//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void sendDhcpMessage(etherHeader *ether, uint8_t type);

// State functions

void setDhcpState(uint8_t state)
{
    dhcpState = state;
}

uint8_t getDhcpState()
{
    return dhcpState;
}

// New address functions
// Manually requested at start-up
// Discover messages sent every 15 seconds

void callbackDhcpGetNewAddressTimer()
{
    /*and this if will be if we have waited for 15 seconds and we are
     * in the slecting state we will go back out to the init state
     * and send antoher discover message again*/

    if (dhcpState == DHCP_SELECTING)
    {
        putsUart0("DHCP timeout. Restarting...\n");
        dhcpState = DHCP_INIT;
        putsUart0("State changed to INIT\r\n");
    }
}

void requestDhcpNewAddress()
{
}

// Renew functions

/*can also call it from the terminal and puts it in renewing state if not it will
 * print that out*/
void renewDhcp()
{
    uint8_t ip[4];
    getIpAddress(ip);

    if (dhcpState == DHCP_BOUND &&
        !(ip[0]==0 && ip[1]==0 && ip[2]==0 && ip[3]==0))
    {
        putsUart0("Manual DHCP renew requested\n");

        //xid++;
        dhcpState = DHCP_RENEWING;
        requestNeeded = true;
    }
    else
    {
        putsUart0("Cannot renew: DHCP not bound\n");
    }
}

// I dont use periodic timers
void callbackDhcpT1PeriodicTimer()
{
}

/*T1 timer will renew for t1 timer*/
void callbackDhcpT1HitTimer()
{
    uint8_t ip[4];
    getIpAddress(ip);

    if (dhcpState == DHCP_BOUND &&
        !(ip[0]==0 && ip[1]==0 && ip[2]==0 && ip[3]==0))
    {
        dhcpState = DHCP_RENEWING;
        //xid++;

        putsUart0("T1 hit. Renewing...\n");

        requestNeeded = true;
    }

}

// Rebind functions

void rebindDhcp()
{
}

void callbackDhcpT2PeriodicTimer()
{
}

// have the t2 timer that just switches staes and makes requested true
void callbackDhcpT2HitTimer()
{
        if (dhcpState == DHCP_RENEWING)
        {
            dhcpState = DHCP_REBINDING;

            putsUart0("T2 hit. Rebinding...\n");

            requestNeeded = true;
        }
}

// End of lease timer
/*if both t1 and t2 failed it will do this lease end
 * and will go back to init and will make requested false */
void callbackDhcpLeaseEndTimer()
{
    if (dhcpState == DHCP_BOUND ||
        dhcpState == DHCP_RENEWING ||
        dhcpState == DHCP_REBINDING)
    {
        putsUart0("Lease expired. Restarting DHCP\n");

        stopTimer(callbackDhcpT1HitTimer);
        stopTimer(callbackDhcpT2HitTimer);
        stopTimer(callbackDhcpLeaseEndTimer);

        uint8_t zeroIp[4] = {0,0,0,0};
        setIpAddress(zeroIp);

        requestNeeded = false;

        dhcpState = DHCP_INIT;
        putsUart0("State changed to INIT\n");
    }
}

// Release functions

void releaseDhcp()
{
    releaseNeeded = true;
}

// IP conflict detection
/*like this is for confilct so after it passes it goes here and turns the flag off
 * stops timers for safety and then has the leases and starts the leases and makes the state to bound*/
void callbackDhcpIpConflictWindow()
{
    putsUart0("IP conflict test passed\n");

    ipConflictDetectionMode = false;

    stopTimer(callbackDhcpT1HitTimer);
    stopTimer(callbackDhcpT2HitTimer);
    stopTimer(callbackDhcpLeaseEndTimer);

    leaseT1 = leaseSeconds / 2;
    leaseT2 = (leaseSeconds * 7) / 8;

    startOneshotTimer(callbackDhcpT1HitTimer, leaseT1);
    startOneshotTimer(callbackDhcpT2HitTimer, leaseT2);
    startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds);

    dhcpState = DHCP_BOUND;

    putsUart0("State changed to BOUND\n");
}

void requestDhcpIpConflictTest()
{

}

bool isDhcpIpConflictDetectionMode()
{
    return ipConflictDetectionMode;
}

// Lease functions

uint32_t getDhcpLeaseSeconds()
{
    return leaseSeconds;
}

// Determines whether packet is DHCP
// Must be a UDP packet
// checks the packet and sees of its from port 67 to port 68
bool isDhcpResponse(etherHeader* ether)
{
    if (!isUdp(ether))
        return false;

    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);

    // DHCP server sends from port 67 to port 68
    if (ntohs(udp->sourcePort) == 67 &&
        ntohs(udp->destPort) == 68)
        return true;

    return false;
}

// Send DHCP message
// Big funciton for all the sending of the packets
void sendDhcpMessage(etherHeader *ether, uint8_t type)
{
    // This is mini state machine
    if (type == DHCPDISCOVER)
        putsUart0("Sending DHCP Discover...\n");
    else if (type == DHCPREQUEST)
        putsUart0("Sending DHCP Request...\n");
    else if (type == DHCPRELEASE)
        putsUart0("Sending DHCP Release...\n");

    socket s;

    // Setup socket ports
    s.localPort = 68;
    s.remotePort = 67;

    // Broadcast MAC
    int i;
    for (i = 0; i < 6; i++)
        s.remoteHwAddress[i] = 0xFF;

    // Broadcast IP
    s.remoteIpAddress[0] = 255;
    s.remoteIpAddress[1] = 255;
    s.remoteIpAddress[2] = 255;
    s.remoteIpAddress[3] = 255;

    // If renewing, send directly to server
    if (dhcpState == DHCP_RENEWING)
    {
        s.remoteIpAddress[0] = dhcpServerIpAdd[0];
        s.remoteIpAddress[1] = dhcpServerIpAdd[1];
        s.remoteIpAddress[2] = dhcpServerIpAdd[2];
        s.remoteIpAddress[3] = dhcpServerIpAdd[3];
    }

    if (type == DHCPRELEASE)
    {
        s.remoteIpAddress[0] = dhcpServerIpAdd[0];
        s.remoteIpAddress[1] = dhcpServerIpAdd[1];
        s.remoteIpAddress[2] = dhcpServerIpAdd[2];
        s.remoteIpAddress[3] = dhcpServerIpAdd[3];
    }


    // Build DHCP Payload Manually


    uint8_t dhcpPayload[300];
    uint16_t index = 0;

    // Zero entire buffer manually
    int y;
    for (y = 0; y < 300; y++)
        dhcpPayload[y] = 0;

    //  BOOTP HEADER

    dhcpPayload[index++] = 1;        // op = BOOTREQUEST
    dhcpPayload[index++] = 1;        // htype = Ethernet
    dhcpPayload[index++] = 6;        // hlen = MAC length
    dhcpPayload[index++] = 0;        // hops

    //xid++;                           // transaction ID

    dhcpPayload[index++] = (xid >> 24) & 0xFF;
    dhcpPayload[index++] = (xid >> 16) & 0xFF;
    dhcpPayload[index++] = (xid >> 8) & 0xFF;
    dhcpPayload[index++] = xid & 0xFF;

    dhcpPayload[index++] = 0;        // secs high
    dhcpPayload[index++] = 0;        // secs low

    // Flags field
    // Flags field
    if (type == DHCPDISCOVER)
    {
        dhcpPayload[index++] = 0x80;
        dhcpPayload[index++] = 0x00;
    }
    else if (type == DHCPREQUEST)
    {
        if (dhcpState == DHCP_RENEWING)
        {
            // Unicast renew
            dhcpPayload[index++] = 0x00;
            dhcpPayload[index++] = 0x00;
        }
        else
        {
            // Broadcast for SELECTING or REBINDING
            dhcpPayload[index++] = 0x80;
            dhcpPayload[index++] = 0x00;
        }
    }
    else
    {
        dhcpPayload[index++] = 0x00;
        dhcpPayload[index++] = 0x00;
    }

    // ciaddr 4 bytes
    if ((type == DHCPRELEASE) ||
        (type == DHCPREQUEST && dhcpState == DHCP_RENEWING))
    {
        uint8_t currentIp[4];
        getIpAddress(currentIp);

        dhcpPayload[index++] = currentIp[0];
        dhcpPayload[index++] = currentIp[1];
        dhcpPayload[index++] = currentIp[2];
        dhcpPayload[index++] = currentIp[3];
    }
    else
    {
        index += 4;
    }

    // yiaddr 4 bytes
    index += 4;

    // siaddr 4 bytes
    index += 4;

    // giaddr 4 bytes
    index += 4;

    // chaddr 16 bytes total
    uint8_t mac[6];
    getEtherMacAddress(mac);
    int z;
    for (z = 0; z < 6; z++)
        dhcpPayload[index++] = mac[z];

    index += 10;  // remaining chaddr padding

    index += 64;  // sname
    index += 128; // file

    //  MAGIC COOKIE
    dhcpPayload[index++] = 0x63;
    dhcpPayload[index++] = 0x82;
    dhcpPayload[index++] = 0x53;
    dhcpPayload[index++] = 0x63;

    //  OPTION 53 Message Type
    dhcpPayload[index++] = 53;
    dhcpPayload[index++] = 1;
    dhcpPayload[index++] = type;

    //  If this is a REQUEST include extra options
    if (type == DHCPREQUEST &&
       (dhcpState == DHCP_SELECTING ||
        dhcpState == DHCP_REQUESTING ||
        dhcpState == DHCP_REBINDING))
    {
        dhcpPayload[index++] = 50;
        dhcpPayload[index++] = 4;

        if (forceNakTest)
        {
            putsUart0("TEST: Requesting wrong IP to force NAK\n");

            dhcpPayload[index++] = 192;
            dhcpPayload[index++] = 168;
            dhcpPayload[index++] = 1;
            dhcpPayload[index++] = 5;   // invalid IP

            forceNakTest = false;
        }
        else
        {
            dhcpPayload[index++] = dhcpOfferedIpAdd[0];
            dhcpPayload[index++] = dhcpOfferedIpAdd[1];
            dhcpPayload[index++] = dhcpOfferedIpAdd[2];
            dhcpPayload[index++] = dhcpOfferedIpAdd[3];
        }
    }

    if (type == DHCPREQUEST &&
        (dhcpState == DHCP_SELECTING ||
         dhcpState == DHCP_REQUESTING))
    {
        // Only include server identifier during initial request
        dhcpPayload[index++] = 54;
        dhcpPayload[index++] = 4;
        dhcpPayload[index++] = dhcpServerIpAdd[0];
        dhcpPayload[index++] = dhcpServerIpAdd[1];
        dhcpPayload[index++] = dhcpServerIpAdd[2];
        dhcpPayload[index++] = dhcpServerIpAdd[3];
    }

    //  OPTION 255 End
    dhcpPayload[index++] = 255;


    sendUdpMessage(ether, s, dhcpPayload, index);
}

uint8_t* getDhcpOption(etherHeader *ether, uint8_t option, uint8_t* length)
{
    return 0;
}

// Determines whether packet is DHCP offer response to DHCP discover
// Must be a UDP packet
bool isDhcpOffer(etherHeader *ether, uint8_t ipOfferedAdd[])
{
    // if not ethter it wont run this
    if (!isDhcpResponse(ether))
        return false;

    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);

    // UDP header is always 8 bytes
    uint8_t *dhcp = (uint8_t*)udp + 8;
    uint8_t *options = dhcp + 240;

    // Verify transaction ID
    uint32_t packetXid =
        (dhcp[4] << 24) |
        (dhcp[5] << 16) |
        (dhcp[6] << 8)  |
         dhcp[7];

    if (packetXid != xid)
        return false;

    // Verify MAC
    uint8_t mac[6];
    getEtherMacAddress(mac);

    uint8_t *chaddr = dhcp + 28;

    int i;
    for (i = 0; i < 6; i++)
    {
        if (chaddr[i] != mac[i])
            return false;
    }

    uint8_t messageType = 0;

    uint8_t *end = (uint8_t*)udp + ntohs(udp->length);

    while (*options != 255 && options < ((uint8_t*)udp + ntohs(udp->length)))
    {
        if (*options == 0)
        {
            options++;
            continue;
        }


        if (options + 1 >= end)
            break;

        uint8_t opt = options[0];
        uint8_t len = options[1];

        if (opt == 53 && len == 1)
        {
            messageType = options[2];
        }

        if (opt == 54 && len == 4)
        {
            dhcpServerIpAdd[0] = options[2];
            dhcpServerIpAdd[1] = options[3];
            dhcpServerIpAdd[2] = options[4];
            dhcpServerIpAdd[3] = options[5];
        }

        options += len + 2;
    }

    if (messageType != DHCPOFFER)
        return false;

    uint8_t *yiaddr = dhcp + 16;

    ipOfferedAdd[0] = yiaddr[0];
    ipOfferedAdd[1] = yiaddr[1];
    ipOfferedAdd[2] = yiaddr[2];
    ipOfferedAdd[3] = yiaddr[3];

    return true;
}
// Determines whether packet is DHCP ACK response to DHCP request
// Must be a UDP packet
/*this verifys the xid and the mac*/
bool isDhcpAck(etherHeader *ether)
{
    if (!isDhcpResponse(ether))
        return false;

    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    uint8_t *dhcp = (uint8_t*)udp + 8;   // UDP header is 8 bytes
    uint8_t *options = dhcp + 240;

    // Verify transaction ID
    uint32_t packetXid =
        (dhcp[4] << 24) |
        (dhcp[5] << 16) |
        (dhcp[6] << 8)  |
         dhcp[7];

    if (packetXid != xid)
        return false;

    // Verify MAC
    uint8_t mac[6];
    getEtherMacAddress(mac);

    uint8_t *chaddr = dhcp + 28;

    int i;
    for (i = 0; i < 6; i++)
    {
        if (chaddr[i] != mac[i])
            return false;
    }

    uint8_t messageType = 0;

    while (*options != 255)
    {
        if (*options == 0)
        {
            options++;
            continue;
        }

        uint8_t opt = options[0];
        uint8_t len = options[1];

        if (opt == 53)
            messageType = options[2];

        options += len + 2;
    }

    return (messageType == DHCPACK);
}

// Handle a DHCP ACK
void handleDhcpAck(etherHeader *ether)
{
    requestNeeded = false;

    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);

    uint8_t *dhcp = (uint8_t*)udp + 8;
    uint8_t *options = dhcp + 240;

    while (*options != 255)
    {
        if (*options == 0)
        {
            options++;
            continue;
        }

        uint8_t opt = options[0];
        uint8_t len = options[1];

        if (opt == 51 && len == 4)
        {
            leaseSeconds =
                (options[2] << 24) |
                (options[3] << 16) |
                (options[4] << 8)  |
                 options[5];

            leaseSeconds = 30;
        }

        if (opt == 54 && len == 4)
        {
            dhcpServerIpAdd[0] = options[2];
            dhcpServerIpAdd[1] = options[3];
            dhcpServerIpAdd[2] = options[4];
            dhcpServerIpAdd[3] = options[5];
        }

        if (opt == 3 && len >= 4)
        {
            gatewayIp[0] = options[2];
            gatewayIp[1] = options[3];
            gatewayIp[2] = options[4];
            gatewayIp[3] = options[5];
        }

        options += len + 2;
    }
    setIpAddress(dhcpOfferedIpAdd);

    // test arp with sendArpRequest and have a little one shot timer
    if (dhcpState == DHCP_REQUESTING)
    {
        putsUart0("Testing IP with ARP...\n");

        ipConflictDetectionMode = true;

        uint8_t zeroIp[4] = {0,0,0,0};
        sendArpRequest(ether, zeroIp, dhcpOfferedIpAdd);

        startOneshotTimer(callbackDhcpIpConflictWindow, 2);
    }
    else
    {
        // I have to stop the timer if not it causes problems
        // and sets it to bound again
        putsUart0("Lease renewed\n");

        stopTimer(callbackDhcpT1HitTimer);
        stopTimer(callbackDhcpT2HitTimer);
        stopTimer(callbackDhcpLeaseEndTimer);

        startOneshotTimer(callbackDhcpT1HitTimer, leaseT1);
        startOneshotTimer(callbackDhcpT2HitTimer, leaseT2);
        startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds);

        dhcpState = DHCP_BOUND;
    }

}

// Here I am adding a new funciton so that
// that i can handle the nak
// but we wont be testing this
bool isDhcpNak(etherHeader *ether)
{
    if (!isDhcpResponse(ether))
        return false;

    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);

    uint8_t *dhcp = (uint8_t*)udp + 8;
    uint8_t *options = dhcp + 240;

    // Verify transaction ID
    uint32_t packetXid =
        (dhcp[4] << 24) |
        (dhcp[5] << 16) |
        (dhcp[6] << 8)  |
         dhcp[7];

    if (packetXid != xid)
        return false;

    // Verify MAC
    uint8_t mac[6];
    getEtherMacAddress(mac);

    uint8_t *chaddr = dhcp + 28;

    int i;
    for (i = 0; i < 6; i++)
    {
        if (chaddr[i] != mac[i])
            return false;
    }

    uint8_t messageType = 0;

    while (*options != 255)
    {
        if (*options == 0)
        {
            options++;
            continue;
        }

        uint8_t opt = options[0];
        uint8_t len = options[1];

        if (opt == 53 && len == 1)
        {
            messageType = options[2];
        }

        options += len + 2;
    }

    return (messageType == DHCPNAK);
}

/*Same for this its so that I can handle the NAK*/
// same will not be doing this
void handleDhcpNak()
{
    putsUart0("DHCP NAK received\n");

    stopTimer(callbackDhcpT1HitTimer);
    stopTimer(callbackDhcpT2HitTimer);
    stopTimer(callbackDhcpLeaseEndTimer);

    uint8_t zeroIp[4] = {0,0,0,0};
    setIpAddress(zeroIp);

    requestNeeded = false;

    dhcpState = DHCP_INIT;

    putsUart0("State changed to INIT\n");
}

// Message requests

bool isDhcpDiscoverNeeded()
{
    return false;
}

bool isDhcpRequestNeeded()
{
    return false;
}

bool isDhcpReleaseNeeded()
{
    return false;
}
// just sets flags and sendDhcpMessage and stop timers
void sendDhcpPendingMessages(etherHeader *ether)
{
    if (releaseNeeded)
    {
        putsUart0("Sending DHCP Release...\n");

        sendDhcpMessage(ether, DHCPRELEASE);

        stopTimer(callbackDhcpT1HitTimer);
        stopTimer(callbackDhcpT2HitTimer);
        stopTimer(callbackDhcpLeaseEndTimer);

        uint8_t zeroIp[4] = {0,0,0,0};
        setIpAddress(zeroIp);

        dhcpState = DHCP_INIT;

        putsUart0("State changed to INIT\n");

        releaseNeeded = false;

        return;
    }

    // as it says if request needed it will do this
    // and set the flag as zero
    if (requestNeeded)
    {
        if (dhcpState == DHCP_RENEWING ||
            dhcpState == DHCP_REBINDING)
        {
            sendDhcpMessage(ether, DHCPREQUEST);
            requestNeeded = false;
        }
    }

    // for the init without sending discover
    if (dhcpState == DHCP_INIT && discoverNeeded)
    {
        xid++;

        sendDhcpMessage(ether, DHCPDISCOVER);

        dhcpState = DHCP_SELECTING;

        putsUart0("State changed to SELECTING\r\n");

        startOneshotTimer(callbackDhcpGetNewAddressTimer, 15);

        discoverNeeded = false;
    }



}

// this is for the start for slecting it will stop timer and send a request
void processDhcpResponse(etherHeader *ether)
{
    if (dhcpState == DHCP_SELECTING)
    {
        if (isDhcpOffer(ether, dhcpOfferedIpAdd))
        {
            stopTimer(callbackDhcpGetNewAddressTimer);

            sendDhcpMessage(ether, DHCPREQUEST);
            dhcpState = DHCP_REQUESTING;

            putsUart0("State changed to REQUESTING\n");
        }
    }
    else if (dhcpState == DHCP_REQUESTING ||
             dhcpState == DHCP_RENEWING ||
             dhcpState == DHCP_REBINDING)
    {
        // ack and nak statements
        if (isDhcpNak(ether))
        {
            handleDhcpNak();
        }
        else if (isDhcpAck(ether))
        {
            handleDhcpAck(ether);
        }
    }
}

void processDhcpArpResponse(etherHeader *ether)
{
    // all the conflict and arp stuff dont really need for demo but
    // still works and finds a new ip
    if (!ipConflictDetectionMode)
        return;

    if (isArpResponse(ether))
    {
        arpPacket *arp = (arpPacket*)ether->data;

        uint8_t myIp[4];
        getIpAddress(myIp);

        if (ntohs(arp->op) == 2 &&
            arp->destIp[0] == myIp[0] &&
            arp->destIp[1] == myIp[1] &&
            arp->destIp[2] == myIp[2] &&
            arp->destIp[3] == myIp[3])
        {
            if (arp->sourceIp[0] == dhcpOfferedIpAdd[0] &&
                arp->sourceIp[1] == dhcpOfferedIpAdd[1] &&
                arp->sourceIp[2] == dhcpOfferedIpAdd[2] &&
                arp->sourceIp[3] == dhcpOfferedIpAdd[3])
            {
                putsUart0("IP CONFLICT DETECTED!\n");

                sendDhcpMessage(ether, DHCPDECLINE);

                uint8_t zeroIp[4] = {0,0,0,0};
                setIpAddress(zeroIp);

                ipConflictDetectionMode = false;

                dhcpState = DHCP_INIT;

                putsUart0("Restarting DHCP\n");
            }
        }
    }
}
// DHCP control functions

// changed it so that the init dont send a discover
void enableDhcp()
{
    uint8_t mac[6];
    getEtherMacAddress(mac);

    srand(mac[5]);   // last byte usually unique
    xid = ((uint32_t)rand() << 16) | rand();

    dhcpEnabled = true;
    dhcpState = DHCP_INIT;
    discoverNeeded = true;
}

void disableDhcp()
{
    dhcpEnabled = false;
    dhcpState = DHCP_DISABLED;
}

bool isDhcpEnabled()
{
    return dhcpEnabled;
}
