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
#include "dhcp.h"
#include "arp.h"
#include "timer.h"
#include <string.h>
#include "ip.h"
#include "udp.h"
#include <stdlib.h>
#include <time.h>

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

// Forward declarations for timer callbacks
void callbackDhcpT1HitTimer();
void callbackDhcpT2HitTimer();
void callbackDhcpLeaseEndTimer();
void callbackDhcpGetNewAddressTimer();
void callbackDhcpIpConflictWindow();

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

uint32_t xid = 0;
uint32_t leaseSeconds = 0;
uint32_t leaseT1 = 0;
uint32_t leaseT2 = 0;

// use these variables if you want
bool discoverNeeded = false;
bool requestNeeded = false;
bool releaseNeeded = false;

bool ipConflictDetectionMode = false;

uint8_t dhcpOfferedIpAdd[4];
uint8_t dhcpServerIpAdd[4];

uint8_t dhcpState = DHCP_DISABLED;
bool    dhcpEnabled = true;

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

// Magic cookie: 99, 130, 83, 99
uint8_t dhcpMagicCookie[] = {0x63, 0x82, 0x53, 0x63};
//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

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
    // If we are still in SELECTING (waiting for offer), go back to INIT
        if (dhcpState == DHCP_SELECTING)
        {
            dhcpState = DHCP_INIT;
            // The next call to sendDhcpPendingMessages will now trigger a new Discover
        }
}

void requestDhcpNewAddress()
{
    // Clear any existing IP info
        uint8_t zeroIp[4] = {0,0,0,0};
        setIpAddress(zeroIp);
        srand(time(NULL));
        // Stop any active timers that might be running from a previous lease
        stopTimer(callbackDhcpT1HitTimer);
        stopTimer(callbackDhcpT2HitTimer);
        stopTimer(callbackDhcpLeaseEndTimer);
        stopTimer(callbackDhcpGetNewAddressTimer);

        // Set state to INIT to trigger sendDhcpPendingMessages
        dhcpEnabled = true;
        setDhcpState(DHCP_INIT);
}

// Renew functions

void renewDhcp()
{
    if (dhcpState == DHCP_BOUND)
        {
            setDhcpState(DHCP_RENEWING);
            requestNeeded = true;
        }
}

void callbackDhcpT1PeriodicTimer()
{

}

void callbackDhcpT1HitTimer()
{
    setDhcpState(DHCP_RENEWING);
    requestNeeded = true;
    startOneshotTimer(callbackDhcpT2HitTimer, leaseT2 - leaseT1);
}

// Rebind functions

void rebindDhcp()
{
    if (dhcpState == DHCP_RENEWING || dhcpState == DHCP_BOUND)
        {
            setDhcpState(DHCP_REBINDING);
            requestNeeded = true;
        }
}

void callbackDhcpT2PeriodicTimer()
{

}

void callbackDhcpT2HitTimer()
{
    setDhcpState(DHCP_REBINDING);
    requestNeeded = true;
    startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds - leaseT2);
}

// End of lease timer
void callbackDhcpLeaseEndTimer()
{
    stopTimer(callbackDhcpT1HitTimer);
    stopTimer(callbackDhcpT2HitTimer);

    uint8_t zeroIp[4] = {0,0,0,0};
    setIpAddress(zeroIp);
    requestNeeded = false;
    dhcpState = DHCP_INIT;
}

// Release functions

void releaseDhcp()
{
    if (dhcpState == DHCP_BOUND || dhcpState == DHCP_RENEWING)
     {
         releaseNeeded = true;  // sendDhcpPendingMessages will send when ether is valid
     }
}

// IP conflict detection

void callbackDhcpIpConflictWindow()
{
    if (getDhcpState() == DHCP_TESTING_IP)
    {
        // 1. Transition to BOUND
        setDhcpState(DHCP_BOUND);

        // 2. Start the T1 Renewal Timer (50% of lease)
        // This is usually a long time (e.g., 1800s for a 1-hour lease)
        startOneshotTimer(callbackDhcpT1HitTimer, leaseT2);
        startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds);

    }
}

void requestDhcpIpConflictTest(etherHeader *ether)
{
    if (getDhcpState() == DHCP_REQUESTING || getDhcpState() == DHCP_TESTING_IP)
    {
        setDhcpState(DHCP_TESTING_IP);
        uint8_t zeroIp[4] = {0, 0, 0, 0};
        sendArpRequest(ether, zeroIp, dhcpOfferedIpAdd);
        startOneshotTimer(callbackDhcpIpConflictWindow, 2);
    }
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
bool isDhcpResponse(etherHeader* ether)
{
    uint8_t* rawData;
    udpHeader* udp;
    ipHeader* ip;

    // Must be an IP packet with valid frame type
    if (ether->frameType != htons(TYPE_IP)) return false;

    // Must be UDP protocol
    ip = (ipHeader*)ether->data;
    if (ip->protocol != PROTOCOL_UDP) return false;

    // Must be from port 67 (DHCP server) to port 68 (DHCP client)
    udp = (udpHeader*)((uint8_t*)ip + ip->size * 4);
    if (udp->sourcePort != htons(67)) return false;
    if (udp->destPort   != htons(68)) return false;

    // Check magic cookie at offset 236 into DHCP payload
    rawData = (uint8_t*)udp + sizeof(udpHeader);
    if (rawData[236] == 0x63 && rawData[237] == 0x82 &&
        rawData[238] == 0x53 && rawData[239] == 0x63)
    {
        return true;
    }

    return false;
}

// Send DHCP message
void sendDhcpMessage(etherHeader *ether, uint8_t type)
{
    if (ether == NULL) return;

    uint8_t i;
    uint8_t macAdd[6];
    uint16_t offset;

    // Get pointer to DHCP payload area
    // Note: IP header is size=5 by default so this offset is valid
    ipHeader* ip = (ipHeader*)ether->data;
    ip->size = 5;  // ensure getUdpData works correctly before sendUdpMessage
    uint8_t* rawData = (uint8_t*)getUdpData(ether);

    // Clear DHCP buffer
    for (offset = 0; offset < 300; offset++)
        rawData[offset] = 0;

    // BOOTP Header
    rawData[0] = 1;   // Message type: Boot Request
    rawData[1] = 1;   // Hardware type: Ethernet
    rawData[2] = 6;   // Hardware address length

    // Transaction ID
    rawData[4] = (xid >> 24) & 0xFF;
    rawData[5] = (xid >> 16) & 0xFF;
    rawData[6] = (xid >> 8)  & 0xFF;
    rawData[7] =  xid        & 0xFF;

    // Broadcast flag
    rawData[10] = 0x80;

    // Client MAC address at offset 28
    getEtherMacAddress(macAdd);
    for (i = 0; i < 6; i++)
        rawData[28 + i] = macAdd[i];

    // Magic cookie
    rawData[236] = 0x63;
    rawData[237] = 0x82;
    rawData[238] = 0x53;
    rawData[239] = 0x63;

    // DHCP Options starting at offset 240
    offset = 240;

    // Option 53: Message Type
    rawData[offset++] = 53;
    rawData[offset++] = 1;
    rawData[offset++] = type;

    // Option 50 & 54: Only for Request or Decline
    if (type == DHCPREQUEST || type == DHCPDECLINE)
    {
        rawData[offset++] = 50;  // Requested IP Address
        rawData[offset++] = 4;
        for (i = 0; i < 4; i++)
            rawData[offset++] = dhcpOfferedIpAdd[i];

        rawData[offset++] = 54;  // Server Identifier
        rawData[offset++] = 4;
        for (i = 0; i < 4; i++)
            rawData[offset++] = dhcpServerIpAdd[i];
    }

    // Option 255: End
    rawData[offset++] = 255;

    // UDP socket setup
    socket s;
    memset(&s, 0, sizeof(socket));
    s.localPort  = 68;
    s.remotePort = 67;

    if (dhcpState == DHCP_RENEWING)
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
            s.remoteIpAddress[i] = dhcpServerIpAdd[i];
        for (i = 0; i < HW_ADD_LENGTH; i++)
            s.remoteHwAddress[i] = 0xFF;
    }
    else
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
            s.remoteIpAddress[i] = 255;
        for (i = 0; i < HW_ADD_LENGTH; i++)
            s.remoteHwAddress[i] = 0xFF;
    }

    // sendUdpMessage handles all ethernet/IP/UDP header setup internally
    sendUdpMessage(ether, s, rawData, offset);
}


uint8_t* getDhcpOption(etherHeader *ether, uint8_t option, uint8_t* length)
{
    uint8_t* rawData = (uint8_t*)getUdpData(ether);
        uint16_t index = 240; // Start after Magic Cookie

        while (index < 1024 && rawData[index] != 0xFF) // 0xFF is End Option
        {
            if (rawData[index] == option)
            {
                *length = rawData[index + 1];
                return &rawData[index + 2];
            }
            // Move to next option: current index + 1 (type) + 1 (len) + length value
            index += 2 + rawData[index + 1];
        }
        return NULL;
}

// Determines whether packet is DHCP offer response to DHCP discover
// Must be a UDP packet
bool isDhcpOffer(etherHeader *ether, uint8_t ipOfferedAdd[])
{
    uint8_t* rawData = (uint8_t*)getUdpData(ether);
    uint32_t receivedXid;
    uint8_t* typeOption;
    uint8_t len;

    // 1. Verify Transaction ID (Bytes 4-7)
    receivedXid = (uint32_t)rawData[4] << 24 | (uint32_t)rawData[5] << 16 |
                      (uint32_t)rawData[6] << 8  | (uint32_t)rawData[7];

    if (receivedXid != xid) return false;

    // 2. Verify DHCP Message Type is DHCPOFFER (Option 53)
    typeOption = getDhcpOption(ether, 53, &len);
    if (typeOption == NULL || *typeOption != DHCPOFFER) return false;

    // 3. Extract the offered IP (Your IP - 'yiaddr' at Bytes 16-19)
    uint8_t i;
    for (i = 0; i < 4; i++)
        ipOfferedAdd[i] = rawData[16 + i];

    return true;
}

// Determines whether packet is DHCP ACK response to DHCP request
// Must be a UDP packet
bool isDhcpAck(etherHeader *ether)
{
    uint8_t* rawData = (uint8_t*)getUdpData(ether);
    uint32_t receivedXid;
    uint8_t* typeOption;
    uint8_t len;

    // 1. Verify Transaction ID matches our current XID
    receivedXid = (uint32_t)rawData[4] << 24 | (uint32_t)rawData[5] << 16 |
                  (uint32_t)rawData[6] << 8  | (uint32_t)rawData[7];

    if (receivedXid != xid) return false;

    // 2. Check if Message Type (Option 53) is DHCPACK
    typeOption = getDhcpOption(ether, 53, &len);
    if (typeOption != NULL && *typeOption == DHCPACK)
    {
        return true;
    }

    return false;
}

// Handle a DHCP ACK
void handleDhcpAck(etherHeader *ether)
{
    ipHeader* ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader* udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    uint8_t* dhcp = (uint8_t*)udp + 8;  // UDP header is always 8 bytes
    uint8_t* options = dhcp + 240;      // options start after 240-byte BOOTP header

    // Extract yiaddr (offered IP at offset 16)
    uint8_t i;
    uint8_t myIp[4];
    for (i = 0; i < 4; i++)
        myIp[i] = dhcp[16 + i];
    setIpAddress(myIp);

    // Walk options
    while (*options != 0xFF)
    {
        if (*options == 0) { options++; continue; }

        uint8_t opt = options[0];
        uint8_t len = options[1];

        if (opt == 1 && len == 4)   // Subnet mask
            setIpSubnetMask(&options[2]);

        if (opt == 3 && len >= 4)   // Gateway
            setIpGatewayAddress(&options[2]);

        if (opt == 6 && len >= 4)   // DNS
            setIpDnsAddress(&options[2]);

        if (opt == 51 && len == 4)  // Lease time
        {
            leaseSeconds = 60;
            leaseT1 = leaseSeconds / 2;
            leaseT2 = (leaseSeconds * 7) / 8;
        }

        options += len + 2;
    }

    // Start conflict detection
    setDhcpState(DHCP_TESTING_IP);
    startOneshotTimer(callbackDhcpIpConflictWindow, 2);
}

// Message requests

bool isDhcpDiscoverNeeded()
{
    return dhcpEnabled && (dhcpState == DHCP_INIT);
}

bool isDhcpRequestNeeded()
{
    return requestNeeded && (dhcpState == DHCP_REQUESTING || dhcpState == DHCP_RENEWING   || dhcpState == DHCP_REBINDING);
}

bool isDhcpReleaseNeeded()
{
    return releaseNeeded;
}

void sendDhcpPendingMessages(etherHeader *ether)
{
    if (!dhcpEnabled) return;

        if (dhcpState == DHCP_INIT)
        {
            xid = ((uint32_t)rand() << 16) | (uint32_t)rand();
            sendDhcpMessage(ether, DHCPDISCOVER);
            setDhcpState(DHCP_SELECTING);
            startOneshotTimer(callbackDhcpGetNewAddressTimer, 15);
        }
        else if (requestNeeded && (dhcpState == DHCP_REQUESTING ||
                                   dhcpState == DHCP_RENEWING ||
                                   dhcpState == DHCP_REBINDING))
        {
            sendDhcpMessage(ether, DHCPREQUEST);
            requestNeeded = false;
        }
        else if (releaseNeeded)
        {
            sendDhcpMessage(ether, DHCPRELEASE);
            releaseNeeded = false;
            setDhcpState(DHCP_DISABLED);
            dhcpEnabled = false;  // ADD - actually disable DHCP
            uint8_t zeroIp[4] = {0, 0, 0, 0};
            setIpAddress(zeroIp);
            setIpSubnetMask(zeroIp);
            setIpGatewayAddress(zeroIp);
            setIpDnsAddress(zeroIp);
            stopTimer(callbackDhcpT1HitTimer);
            stopTimer(callbackDhcpT2HitTimer);
            stopTimer(callbackDhcpLeaseEndTimer);
            stopTimer(callbackDhcpT1PeriodicTimer);
            stopTimer(callbackDhcpT2PeriodicTimer);
        }
}

void processDhcpResponse(etherHeader *ether)
{
    if (!isDhcpResponse(ether)) return;

    uint8_t state = getDhcpState();
    if (state == DHCP_SELECTING && isDhcpOffer(ether, dhcpOfferedIpAdd))
    {
        stopTimer(callbackDhcpGetNewAddressTimer);

        uint8_t len;
        uint8_t* serverIdOpt = getDhcpOption(ether, 54, &len);
        if (serverIdOpt) memcpy(dhcpServerIpAdd, serverIdOpt, 4);

        setDhcpState(DHCP_REQUESTING);
        requestNeeded = true;
    }
    else if ((state == DHCP_REQUESTING || state == DHCP_RENEWING || state == DHCP_REBINDING) && isDhcpAck(ether))
    {
        // If renewing or rebinding, we successfully updated the lease
        if (state == DHCP_RENEWING || state == DHCP_REBINDING)
        {
            stopTimer(callbackDhcpT1HitTimer);
            stopTimer(callbackDhcpT2HitTimer);
            stopTimer(callbackDhcpLeaseEndTimer);
            setDhcpState(DHCP_BOUND);
            startOneshotTimer(callbackDhcpT1HitTimer, leaseT2);
            startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds);
        }
        else // Initial ACK
        {
            handleDhcpAck(ether);
        }
    }
}

void processDhcpArpResponse(etherHeader *ether)
{
   return;
}

// DHCP control functions

void enableDhcp()
{
    dhcpEnabled = true;
    dhcpState = DHCP_INIT;
}

void disableDhcp()
{
    dhcpEnabled = false;
    setDhcpState(DHCP_DISABLED);
}

bool isDhcpEnabled()
{
    return dhcpEnabled;
}

