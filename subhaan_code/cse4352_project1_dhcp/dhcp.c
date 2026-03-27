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

// Name: Subhaan Elburz
// ID:   1002135522

// ethernet.c will get all of the packets and process them
// how it does it, is that it will receive packets, it will check what type it is
// isDhcpResponse() checks if it is a DHCP message, then it will call processDhcpResponse() if it is
// also, it will check for ARP responses and call processDhcpArpResponse()
// if it gets back a response in the TESTING IP state
// and lastly it will call sendDhcpPendingMessages() is dhcp is enabled to send the actual messages
// DHCP init: ethernet.c will call enableDhcp() if EEPROM is cleared or if dhcp on is entered
//
// So, that is what starts the state machine with the first periodic discover timer
// in the init state, it will set the discover needed flag, then sendDhcpPendingMessages() will send it
// when the offer comes, processDhcpResponse() grabs the server IP and moves to selecting
// it will wait 2s for other offers, then will send request and moves to requesting state
// when the server responds with an ACK it will save all settings and go to testing ip
// where it will send ARP test probe to check if its taken, if no response then
// we apply the given ip, start all the lease timers, and enter bound
// then depending on T1 and T2 we will enter renewing or rebinding, explained more in code

#include <stdio.h>
#include "dhcp.h"
#include "arp.h"
#include "timer.h"

// all of the DHCP message types from rfc 2131
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7
#define DHCPINFORM   8

// all of the possible states for our DHCP state machine
#define DHCP_DISABLED   0
#define DHCP_INIT       1
#define DHCP_SELECTING  2
#define DHCP_REQUESTING 3
#define DHCP_TESTING_IP 4   // extra state added so we dont have ip conflicts
#define DHCP_BOUND      5
#define DHCP_RENEWING   6
#define DHCP_REBINDING  7
#define DHCP_INITREBOOT 8   // not used since ip not stored over reboot
#define DHCP_REBOOTING  9   // not used since ip not stored over reboot

// these are the durations for all of the timers
#define DISCOVER_TIMER  15  // if we get ghosted we will send another discover after 15s
#define RENEW_TIMER     5   // if landlord ignores our renew, resend in 5s
#define REBIND_TIMER    5   // if begging any server fails, try again in 5s
#define OFFER_TIMEOUT   2   // wait 2s to see if multiple servers offer us ips
#define ACK_TIMEOUT     5   // if a server ghosts our request, give up after 5s
#define ARP_WINDOW      2   // wait 2s to see if no response, if no response move to bound

// these are the ports for DHCP
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

// this is the magic cookie which tells us its DHCP not bootP
#define DHCP_MAGIC_COOKIE 0x63825363

// these are the DHCP options from rfc 2132
#define DHCP_SUBNET_MASK    1
#define DHCP_ROUTER         3
#define DHCP_DNS            6
#define DHCP_REQ_IP         50
#define DHCP_LEASE_TIME     51
#define DHCP_MSG_TYPE       53
#define DHCP_SERVER_ID      54
#define DHCP_PARAM_LIST     55
#define DHCP_CLIENT_ID      61
#define DHCP_END            255

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

uint32_t xid = 0;               // this will be the transaction id
uint32_t leaseSeconds = 0;      // this is the total lease time from the server
uint32_t leaseT1 = 0;           // renewal time with landlord (T1 = 50% of lease)
uint32_t leaseT2 = 0;           // rebind time with everybody (T2 = 87.5% of lease)

bool discoverNeeded = false;    // flag to indicate discover needed
bool requestNeeded = false;     // flag to indicate when a request for an ip is needed
bool releaseNeeded = false;     // flag to indicate when we need to release the ip

bool ipConflictDetectionMode = false;   // flag for testing ip when we check for ip conflicts

uint8_t dhcpOfferedIpAdd[4];    // the ip address the server is offering to us
uint8_t dhcpServerIpAdd[4];     // the landlord's (dhcp server) ip address

uint8_t dhcp_server_hw_add[6];      // the server/landlord mac address
uint8_t dhcp_offered_sn_mask[4];    // the subnet mask from the server
uint8_t dhcp_offered_gw_add[4];     // the router gateway address
uint8_t dhcp_offered_dns_add[4];    // the dns server address

uint8_t dhcpState = DHCP_DISABLED;  // the current state in the state machine
bool dhcpEnabled = false;            // flag to turn dhcp on/off

// had to add function definitions to compile properly
void callbackDhcpGetNewAddressTimer();
void callback_offer_timeout();
void callback_ack_timeout();
void callbackDhcpIpConflictWindow();
void callbackDhcpT1HitTimer();
void callbackDhcpT1PeriodicTimer();
void callbackDhcpT2HitTimer();
void callbackDhcpT2PeriodicTimer();
void callbackDhcpLeaseEndTimer();

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void kill_dhcp_timers()
{
    // literally just stop all 9 timers
    stopTimer(callbackDhcpGetNewAddressTimer);
    stopTimer(callback_offer_timeout);
    stopTimer(callback_ack_timeout);
    stopTimer(callbackDhcpIpConflictWindow);
    stopTimer(callbackDhcpT1HitTimer);
    stopTimer(callbackDhcpT1PeriodicTimer);
    stopTimer(callbackDhcpT2HitTimer);
    stopTimer(callbackDhcpT2PeriodicTimer);
    stopTimer(callbackDhcpLeaseEndTimer);
}

// this is the first state of the state machine where
// everything is reset and DHCP is first turned on
void init_state1()
{
    // first thing we do is actually kill all timers in case we dropped to this state
    kill_dhcp_timers();

    // second thing in the state machine is setting the local ip to 0.0.0.0
    uint8_t local_ip[IP_ADD_LENGTH] = {0,0,0,0};
    setIpAddress(local_ip);

    // then set flags
    requestNeeded = false;
    releaseNeeded = false;
    ipConflictDetectionMode = false;
    discoverNeeded = true;  // we only need to send periodic discover message timer so enable

    // then we can set the transaction id to a random value
    xid = random32();

    // then we can finally change the state and start the discover timer
    dhcpState = DHCP_INIT;
    startPeriodicTimer(callbackDhcpGetNewAddressTimer, DISCOVER_TIMER);
}

// State functions

void setDhcpState(uint8_t state)
{
    dhcpState = state;  // sets the dhcp state
}

uint8_t getDhcpState()
{
    return dhcpState;   // gets the dhcp state
}

// New address functions
// Manually requested at start-up
// Discover messages sent every 15 seconds

void callbackDhcpGetNewAddressTimer()
{
    // callback function for discover periodically sending every 15 seconds
    discoverNeeded = true;
}

void callback_offer_timeout()
{
    // the waiting time for offers is over so now we
    // flag the main loop to request the ip we picked
    requestNeeded = true;
}

void callback_ack_timeout()
{
    // the server never replied to our request
    // so we go back to init
    init_state1();
}

void requestDhcpNewAddress()
{
    // when we request a new ip address from DHCP server
    // we enter the init state of the state machine
    init_state1();
}

// Renew functions

void renewDhcp()
{
    // this is the manual command the user types
    // it will force the state to go to renewing
    if (dhcpState == DHCP_BOUND)
    {
        stopTimer(callbackDhcpT1HitTimer);
        dhcpState = DHCP_RENEWING;
        requestNeeded = true;
        startPeriodicTimer(callbackDhcpT1PeriodicTimer, RENEW_TIMER);
    }
}

void callbackDhcpT1PeriodicTimer()
{
    // this timer will send a periodic renew request
    requestNeeded = true;
}

void callbackDhcpT1HitTimer()
{
    // this meanse that the T1 timer has ended
    // and we have to move to renewing to ask the landlord
    // for more time
    dhcpState = DHCP_RENEWING;
    requestNeeded = true;
    startPeriodicTimer(callbackDhcpT1PeriodicTimer, RENEW_TIMER);
}

// Rebind functions

void rebindDhcp()
{
    // this is whats called when the user forces a rebind
    if ( (dhcpState == DHCP_RENEWING) || (dhcpState == DHCP_BOUND) )
    {
        stopTimer(callbackDhcpT1PeriodicTimer);
        dhcpState = DHCP_REBINDING;
        requestNeeded = true;
        startPeriodicTimer(callbackDhcpT2PeriodicTimer, REBIND_TIMER);
    }
}

void callbackDhcpT2PeriodicTimer()
{
    // this is the timer that sends a periodic broadcast rebind
    requestNeeded = true;
}

void callbackDhcpT2HitTimer()
{
    // this means that T2 is finished and we can no longer
    // renew the request with current landlord, so we broadcast
    // to any and all dhcp servers
    stopTimer(callbackDhcpT1PeriodicTimer);
    dhcpState = DHCP_REBINDING;
    requestNeeded = true;
    startPeriodicTimer(callbackDhcpT2PeriodicTimer, REBIND_TIMER);
}

// End of lease timer
void callbackDhcpLeaseEndTimer()
{
    // when this timer ends that means the entire lease is up
    // so we go back to the init state
    init_state1();
}

// Release functions

void releaseDhcp()
{
    // this is called when user forces to release
    // so if we have an IP, we give it up (ip only in or after bound state)
    if (dhcpState >= DHCP_BOUND)
    {
        releaseNeeded = true;
    }
}

// IP conflict detection

void callbackDhcpIpConflictWindow()
{
    // this means that the ARP window timer finished
    // and no we got no ARP responses, so we move to the
    // bound state
    ipConflictDetectionMode = false;

    // now we can safely apply all of the
    // network settings that the dhcp server gave us
    setIpAddress(dhcpOfferedIpAdd);
    setIpSubnetMask(dhcp_offered_sn_mask);
    setIpGatewayAddress(dhcp_offered_gw_add);
    setIpDnsAddress(dhcp_offered_dns_add);

    // calculate the t1 and t2 times if the server did not send them
    if (leaseT1 == 0)
    {
        leaseT1 = leaseSeconds / 2;
    }

    if (leaseT2 == 0)
    {
        leaseT2 = (leaseSeconds * 7) / 8;
    }

    // now we can start all of the lease timers
    startOneshotTimer(callbackDhcpT1HitTimer, leaseT1);
    startOneshotTimer(callbackDhcpT2HitTimer, leaseT2);
    startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds);

    dhcpState = DHCP_BOUND; // and we finally can be bounded to the server
}

void requestDhcpIpConflictTest()
{
    // if we do get an ARP response that means
    // we cant use that ip address, so we set the flag
    ipConflictDetectionMode = true;
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
bool isDhcpResponse(etherHeader *ether)
{
    // get the ip header and calculate its length
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ip_header_length = ip->size * 4;

    // use the ip header length to find the udp header
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ip_header_length);

    // the dhcp frame is the actual data payload of the udp packet
    dhcpFrame *dhcp = (dhcpFrame*)udp->data;

    // this massive conditional checks if it is a response meant for us
    if ( (dhcp->magicCookie == htonl(DHCP_MAGIC_COOKIE)) && // verify magic cookie not bootP
         (dhcp->xid == htonl(xid)) &&                       // verify the transaction id is same
         (dhcp->op == 2) &&                                 // verify the operation is a reply
         (ntohs(udp->destPort) == DHCP_CLIENT_PORT) &&      // verify destination is to client
         (ntohs(udp->sourcePort) == DHCP_SERVER_PORT)   )   // verify source is the server
    {
        uint8_t local_hw_address[HW_ADD_LENGTH];        // get the mac address
        getEtherMacAddress(local_hw_address);

        uint8_t i;
        for (i = 0; i < HW_ADD_LENGTH; i++)
        {
            if (dhcp->chaddr[i] != local_hw_address[i]) // check if the client mac address is us
            {
                return false;   // if it isnt us return false
            }
        }
        return true;            // if it is us return true
    }

    return false;
}

// Send DHCP message
void sendDhcpMessage(etherHeader *ether, uint8_t type)
{
    // copied exactly like sendUDP but changed a few things for DHCP
    uint8_t i;
    uint16_t j;
    uint32_t sum;
    uint16_t tmp16;
    uint16_t udpLength;
    uint8_t localHwAddress[6];
    uint8_t localIpAddress[4];
    uint16_t options_count = 0;             // we need this to keep track of how big our options array gets

    // Ether frame
    getEtherMacAddress(localHwAddress);     // getting our local mac address
    getIpAddress(localIpAddress);           // getting our local ip address

    // we have to manually figure out the destination mac since no socket
    // so destAddress removed from the first loop
    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        ether->sourceAddress[i] = localHwAddress[i];  // this is our source mac address
    }

    // if we are renewing or releasing that means we know the
    // landlord's mac address, so we unicast directly to it
    if ( (type == DHCPREQUEST && dhcpState == DHCP_RENEWING) || (type == DHCPRELEASE) )
    {
        for (i = 0; i < HW_ADD_LENGTH; i++)
        {
            ether->destAddress[i] = dhcp_server_hw_add[i];  // talk directly to server using saved mac address
        }
    }
    else
    {
        // else we are discovering or rebinding
        // so we just talk to everyone by broadcasting
        for (i = 0; i < HW_ADD_LENGTH; i++)
        {
            ether->destAddress[i] = 0xFF;   // broadcasting is all FFs
        }
    }

    ether->frameType = htons(TYPE_IP);      // set the ethernet frame type for an IP packet

    // IP header
    ipHeader *ip = (ipHeader*)ether->data;
    ip->rev = 0x4;                      // set as ipv4 datagram
    ip->size = 0x5;                     // size is 4*5 = 20 bytes with no options
    ip->typeOfService = 0;
    ip->id = 0;
    ip->flagsAndOffset = 0;
    ip->ttl = 128;                      // set time to live to 128 jumps
    ip->protocol = PROTOCOL_UDP;        // set ip protocol to UDP
    ip->headerChecksum = 0;

    // again the simple for loop has to be split since for the source
    // ip, if we are discovering or requesting, we don't actually have an ip
    if ( (type == DHCPDISCOVER) || (dhcpState == DHCP_SELECTING) || (dhcpState == DHCP_REQUESTING) )
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            ip->sourceIp[i] = 0;        // so we must send from 0.0.0.0 (same as in the init_state1 function)
        }
    }
    else
    {
        // if we are renewing or releasing, we have an ip address,
        // the only problem is its about to expire, but that doesn't matter
        // so we use it anyway
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            ip->sourceIp[i] = localIpAddress[i];
        }
    }

    // the destination ip also has to be unicast directly to the server
    // for renew/release like the mac address since we already have it
    if ( (type == DHCPREQUEST && dhcpState == DHCP_RENEWING) || (type == DHCPRELEASE) )
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            ip->destIp[i] = dhcpServerIpAdd[i]; // used the saved destination ip address
        }
    }
    else
    {
        // else, we dont have a destination ip saved so we
        // have to broadcast to the whole network
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            ip->destIp[i] = 0xFF;       // broadcasting is all FFs
        }
    }

    uint8_t ip_header_length = ip->size * 4;

    // UDP header
    // again, we dont have a socket like UDP, but for DHCP
    // the source and destination ports are already defined
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ip_header_length);
    udp->sourcePort = htons(DHCP_CLIENT_PORT);  // we send from port 68
    udp->destPort = htons(DHCP_SERVER_PORT);    // we send to port 67

    // this replaces the raw data payload from sendUdpMessage
    dhcpFrame *dhcp = (dhcpFrame*)udp->data;

    // the first 240 bytes is garbage from bootP so
    // we just set those bytes all equal to zero
    for (j = 0; j < 240; j++)
    {
        ((uint8_t*)dhcp)[j] = 0;
    }

    dhcp->op = 1;                   // 1 = BOOTREQUEST (meaning client is talking to server)
    dhcp->htype = 1;                // 1 = ethernet
    dhcp->hlen = 6;                 // mac address length is 6 bytes
    dhcp->hops = 0;                 // clients always set this to 0
    dhcp->xid = htonl(xid);         // attach our random transaction id + flip endianness
    dhcp->secs = 0;
    dhcp->flags = htons(0x0000);    // 0x8000 is the broadcast flag, 0x0000 is the unicast flag *CHANGED TO UNICAST FROM SUBMISSION

    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        dhcp->chaddr[i] = localHwAddress[i];    // chaddr = client hardware address
    }

    // ciaddr = client ip address
    // we only fill this if we already own the ip (if not then its 0.0.0.0)
    // and are talking to the landlord about it (renewing/rebinding/release)
    if ( (type == DHCPREQUEST && (dhcpState == DHCP_RENEWING || dhcpState == DHCP_REBINDING)) || (type == DHCPRELEASE) )
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            dhcp->ciaddr[i] = localIpAddress[i];
        }
    }

    // magic cookie is what tells the server it is DHCP and not bootP
    dhcp->magicCookie = htonl(DHCP_MAGIC_COOKIE);

    // dhcp options is basically just a sequences of bytes at the end
    // of the packet, and we use options count to keep track of how
    // many bytes we put at the end of the dhcp packet
    options_count = 0;

    // option 53 is dhcp message type (must be the first option)
    // each option has a code byte, length byte, then the actual data
    dhcp->options[options_count++] = DHCP_MSG_TYPE;
    dhcp->options[options_count++] = 1;             // length of data is 1 byte
    dhcp->options[options_count++] = type;          // the actual type like discover or request

    // option 61 is the client id, so the dhcp message is coming from this id
    // added since was not getting ACK all the time
    dhcp->options[options_count++] = DHCP_CLIENT_ID;
    dhcp->options[options_count++] = 7;
    dhcp->options[options_count++] = 1;
    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        dhcp->options[options_count++] = localHwAddress[i];
    }

    // option 50 is requested ip address
    // required when requesting an offered ip or declining an ip
    if ( (type == DHCPREQUEST && (dhcpState == DHCP_SELECTING || dhcpState == DHCP_REQUESTING)) || (type == DHCPDECLINE) )
    {
        dhcp->options[options_count++] = DHCP_REQ_IP;
        dhcp->options[options_count++] = 4;             // ipv4 addresses are 4 bytes
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            dhcp->options[options_count++] = dhcpOfferedIpAdd[i];
        }
    }

    // option 54 is the server identifier
    // required so the specific dhcp server knows this request, decline, or release is meant for them
    if ( (type == DHCPREQUEST && (dhcpState == DHCP_SELECTING || dhcpState == DHCP_REQUESTING)) || (type == DHCPDECLINE) || (type == DHCPRELEASE) )
    {
        dhcp->options[options_count++] = DHCP_SERVER_ID;
        dhcp->options[options_count++] = 4;
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            dhcp->options[options_count++] = dhcpServerIpAdd[i];
        }
    }

    // option 55 is the parameter request list
    // asks the server to include these specific network parameters in its reply
    if ( (type == DHCPDISCOVER) || (type == DHCPREQUEST) )
    {
        dhcp->options[options_count++] = DHCP_PARAM_LIST;
        dhcp->options[options_count++] = 4;                 // we are specifically requesting 4 options back
        dhcp->options[options_count++] = DHCP_SUBNET_MASK;  // request subnet mask
        dhcp->options[options_count++] = DHCP_ROUTER;       // request router
        dhcp->options[options_count++] = DHCP_DNS;          // request dns server
        dhcp->options[options_count++] = DHCP_LEASE_TIME;   // request lease time
    }

    // option 255 just marks the end of the options byte array
    dhcp->options[options_count++] = DHCP_END;

    // now that we have written all of the options we pad the end
    // so that the checksum math is correct to a 4 byte boundary
    while (options_count % 4 != 0)
    {
        dhcp->options[options_count++] = 0;
    }

    // finish off the packet by doing checksums
    uint16_t dhcp_size = 240 + options_count;           // fixed dhcp (240) + options size
    udpLength = sizeof(udpHeader) + dhcp_size;          // set UDP length to header + payload size
    ip->length = htons(ip_header_length + udpLength);   // set total ip length to ip header + UDP length

    // calculate the full IP checksum
    calcIpChecksum(ip);

    // set udp length
    udp->length = htons(udpLength);

    // calculate the udp checksum using the pseudoheader
    sum = 0;
    sumIpWords(ip->sourceIp, 8, &sum);  // sum source and dest ips for pseudoheader
    tmp16 = ip->protocol;
    sum += (tmp16 & 0xff) << 8;         // add the protocol byte (pseudoheader)
    sumIpWords(&udp->length, 2, &sum);  // add the udp length (pseudoheader)
    udp->check = 0;                     // zero it out before math
    sumIpWords(udp, udpLength, &sum);   // actual summation of UDP header and dhcp data
    udp->check = getIpChecksum(sum);    // finish the UDP checksum

    // send packet with size = ether hdr + ip header + udp + dhcp
    putEtherPacket(ether, sizeof(etherHeader) + ip_header_length + udpLength);
}

// this function goes through the options array to find a
// specific option; option format: code, length, data
// it returns a pointer to the data portion and it sets the length
uint8_t* getDhcpOption(etherHeader *ether, uint8_t option, uint8_t* length)
{
    // get pointers to the ip and udp headers
    // then get the pointer to the dhcp frame
    ipHeader *ip = (ipHeader*) ether->data;
    uint8_t ip_header_length = ip->size * 4;
    udpHeader *udp = (udpHeader*) ((uint8_t*) ip + ip_header_length);
    dhcpFrame *dhcp = (dhcpFrame*) udp->data;

    // calculate how many bytes of options we actually have
    // total payload minus the fixed 240 bytes
    uint16_t udp_data_length = ntohs(udp->length) - sizeof(udpHeader);
    uint16_t options_length = udp_data_length - 240;

    uint16_t i = 0;
    while (i < options_length)
    {
        // get the current option code
        uint8_t code = dhcp->options[i];

        // code 255 means we hit the end of the options
        if (code == DHCP_END)
        {
            break;
        }

        // code 0 is a padding byte
        // just skip it and move to the next byte
        if (code == 0)
        {
            i++;
            continue;
        }

        // the byte right after the code is the length of the data
        uint8_t opt_length = dhcp->options[i + 1];

        // check if this is the option we are looking for
        if (code == option)
        {
            // pass the length back to the calling function
            *length = opt_length;

            // return a pointer to the actual data payload
            return &dhcp->options[i + 2];
        }

        // skip ahead to the next option
        i += 2 + opt_length;
    }

    // return 0 if the option was not found
    *length = 0;
    return 0;
}

// Determines whether packet is DHCP offer response to DHCP discover
// Must be a UDP packet
bool isDhcpOffer(etherHeader *ether, uint8_t ipOfferedAdd[])
{
    uint8_t length = 0;

    // call getDhcpOption to find the message type option
    uint8_t *opt_data = getDhcpOption(ether, DHCP_MSG_TYPE, &length);
    bool ok = false;

    // check if the option exists and has a length of 1 byte
    if (opt_data != 0 && length == 1)
    {
        // verify that the message type is exactly 2 for an offer
        ok = (opt_data[0] == DHCPOFFER);
    }

    if (ok)
    {
        // if it is an offer we need to extract the ip address
        // get pointers to the headers to access the dhcp frame
        ipHeader *ip = (ipHeader*) ether->data;
        uint8_t ip_header_length = ip->size * 4;
        udpHeader *udp = (udpHeader*) ((uint8_t*) ip + ip_header_length);
        dhcpFrame *dhcp = (dhcpFrame*) udp->data;

        uint8_t i;
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            // copy the offered ip address into the provided array
            // so we can request it in the next step
            ipOfferedAdd[i] = dhcp->yiaddr[i];
        }
    }

    return ok;
}

// Determines whether packet is DHCP ACK response to DHCP request
// Must be a UDP packet
bool isDhcpAck(etherHeader *ether)
{
    uint8_t length = 0;
    uint8_t *opt_data = getDhcpOption(ether, DHCP_MSG_TYPE, &length);
    bool ok = false;

    if ( (opt_data != 0) && (length == 1) )
    {
        // check if the message type is 5 (DHCPACK)
        ok = (opt_data[0] == DHCPACK);
    }

    return ok;
}

// same thing but not check if its a rejection
bool is_dhcp_nak(etherHeader *ether)
{
    uint8_t length = 0;
    uint8_t *opt_data = getDhcpOption(ether, DHCP_MSG_TYPE, &length);
    bool ok = false;

    if (opt_data != 0 && length == 1)
    {
        // check if the message type is 6 (DHCPNAK)
        ok = (opt_data[0] == DHCPNAK);
    }

    return ok;
}

// Handle a DHCP ACK
void handleDhcpAck(etherHeader *ether)
{
    // extracts all the lease parameters from a dhcp ack
    // we just save these to variables for now
    // we dont apply them until after test ip finishes
    uint8_t i;
    uint8_t length = 0;
    uint8_t *opt_data;

    // get pointers to all the headers to access the dhcp frame
    ipHeader *ip = (ipHeader*) ether->data;
    uint8_t ip_header_length = ip->size * 4;
    udpHeader *udp = (udpHeader*) ((uint8_t*) ip + ip_header_length);
    dhcpFrame *dhcp = (dhcpFrame*) udp->data;

    // save the ip address the server is assigning to us
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        dhcpOfferedIpAdd[i] = dhcp->yiaddr[i];
    }

    // save the server mac address from the ethernet header
    // we need this to send unicast packets directly to the landlord later
    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        dhcp_server_hw_add[i] = ether->sourceAddress[i];
    }

    // extract the subnet mask from option 1
    opt_data = getDhcpOption(ether, DHCP_SUBNET_MASK, &length);
    if ( (opt_data != 0) && (length == 4) )
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            dhcp_offered_sn_mask[i] = opt_data[i];
        }
    }

    // extract the gateway router from option 3
    opt_data = getDhcpOption(ether, DHCP_ROUTER, &length);
    if ( (opt_data != 0) && (length >= 4) )
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            dhcp_offered_gw_add[i] = opt_data[i];
        }
    }

    // extract the dns server from option 6
    opt_data = getDhcpOption(ether, DHCP_DNS, &length);
    if ( (opt_data != 0) && (length >= 4) )
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            dhcp_offered_dns_add[i] = opt_data[i];
        }
    }

    // extract the server identifier from option 54
    // this is the ip address of our landlord
    opt_data = getDhcpOption(ether, DHCP_SERVER_ID, &length);
    if ( (opt_data != 0) && (length == 4) )
    {
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            dhcpServerIpAdd[i] = opt_data[i];
        }
    }

    // extract the lease time from option 51
    // the data comes as 4 separate bytes in network byte order
    opt_data = getDhcpOption(ether, DHCP_LEASE_TIME, &length);
    if ( (opt_data != 0) && (length == 4) )
    {
        // we are basically manually shifting the 4 separate bytes
        // into one 32 bit integer
        leaseSeconds = ((uint32_t) opt_data[0] << 24)
                | ((uint32_t) opt_data[1] << 16) | ((uint32_t) opt_data[2] << 8)
                | (uint32_t) opt_data[3];
    }

    leaseSeconds = 60; // manually setting total least time to 60s *comment out later

    // then right after we reset the renew and rebind times
    leaseT1 = 0;
    leaseT2 = 0;
}

// Message requests

bool isDhcpDiscoverNeeded()
{
    return discoverNeeded;
}

bool isDhcpRequestNeeded()
{
    return requestNeeded;
}

bool isDhcpReleaseNeeded()
{
    return releaseNeeded;
}

void sendDhcpPendingMessages(etherHeader *ether)
{
    // only send messages when the ethernet link is up
    if (isEtherLinkUp())
    {
        // if we are in the init state and the flag is set
        // we need to broadcast a discover message to find a server
        if (dhcpState == DHCP_INIT && discoverNeeded)
        {
            discoverNeeded = false;
            sendDhcpMessage(ether, DHCPDISCOVER);
        }

        // when the offer timer finishes we are done collecting offers
        // and we formally request the ip that we picked
        if (dhcpState == DHCP_SELECTING && requestNeeded)
        {
            requestNeeded = false;
            sendDhcpMessage(ether, DHCPREQUEST);

            // advance the state machine to requesting
            dhcpState = DHCP_REQUESTING;

            // start a timer to wait for the ack
            // if the server ghosts us we will restart the process
            startOneshotTimer(callback_ack_timeout, ACK_TIMEOUT);
        }

        // when half of our lease time has passed
        // we need to ask our specific landlord to extend the lease
        // also comment this out when testing T1 timer: T1COMMENT
        /*if (dhcpState == DHCP_RENEWING && requestNeeded)
        {
            requestNeeded = false;

            // this gets sent as a unicast message directly to the landlord
            sendDhcpMessage(ether, DHCPREQUEST);
        }*/

        // if the landlord never replied to our renew requests
        // we have to broadcast to any server on the network for help
        // comment this out when testing full lease timer: LEASECOMMENT
        /*if (dhcpState == DHCP_REBINDING && requestNeeded)
        {
            requestNeeded = false;
            sendDhcpMessage(ether, DHCPREQUEST);
        }*/

        // if the user manually requested to drop the ip address
        // we send a release message and clear everything out
        if (releaseNeeded)
        {
            releaseNeeded = false;
            sendDhcpMessage(ether, DHCPRELEASE);

            // go back to the first init state
            init_state1();
        }
    }
}

void processDhcpResponse(etherHeader *ether)
{
    uint8_t i;

    // we are in the init state waiting for offers
    if (dhcpState == DHCP_INIT)
    {
        if (isDhcpOffer(ether, dhcpOfferedIpAdd))
        {
            // we got an offer so we extract the server ip
            uint8_t length = 0;
            uint8_t *server_id = getDhcpOption(ether, DHCP_SERVER_ID, &length);

            if ( (server_id != 0) && (length == 4) )
            {
                for (i = 0; i < IP_ADD_LENGTH; i++)
                {
                    dhcpServerIpAdd[i] = server_id[i];
                }
            }
            else
            {
                // if the server id option is ever missing
                // we can just take it from the ip header
                ipHeader *ip_sid = (ipHeader*) ether->data;

                for (i = 0; i < IP_ADD_LENGTH; i++)
                {
                    dhcpServerIpAdd[i] = ip_sid->sourceIp[i];
                }
            }

            // save the server mac address from the ethernet header
            // we need this to send unicast messages directly to them later
            for (i = 0; i < HW_ADD_LENGTH; i++)
            {
                dhcp_server_hw_add[i] = ether->sourceAddress[i];
            }

            // stop the discover timer and move to the selecting state
            stopTimer(callbackDhcpGetNewAddressTimer);
            dhcpState = DHCP_SELECTING;

            // wait a short period to see if multiple servers respond
            startOneshotTimer(callback_offer_timeout, OFFER_TIMEOUT);
        }
    }

    // we are requesting and waiting for an ack or nak
    else if (dhcpState == DHCP_REQUESTING)
    {
        if (isDhcpAck(ether))
        {
            // the server accepted our request
            stopTimer(callback_ack_timeout);

            // extract all the lease parameters
            handleDhcpAck(ether);

            // we send an arp probe with an ip of zero
            // to check if anyone else is using the ip
            uint8_t test_ip[4] = {0, 0, 0, 0};
            sendArpRequest(ether, test_ip, dhcpOfferedIpAdd);

            // enter the testing ip state and wait for an arp reply
            ipConflictDetectionMode = true;
            dhcpState = DHCP_TESTING_IP;
            startOneshotTimer(callbackDhcpIpConflictWindow, ARP_WINDOW);
        }
        else if (is_dhcp_nak(ether))
        {
            // the server rejected our request
            // start the process over in init state
            init_state1();
        }
    }

    // we are renewing and waiting for a reply from our landlord
    // comment out this function to test timer T2 and see if it fires: T1COMMENT
    /*else if (dhcpState == DHCP_RENEWING)
    {
        if (isDhcpAck(ether))
        {
            // the server extended our lease
            stopTimer(callbackDhcpT1PeriodicTimer);
            stopTimer(callbackDhcpT2HitTimer);
            stopTimer(callbackDhcpLeaseEndTimer);

            // refresh the lease parameters
            handleDhcpAck(ether);

            // apply the settings immediately since we already own the ip
            setIpAddress(dhcpOfferedIpAdd);
            setIpSubnetMask(dhcp_offered_sn_mask);
            setIpGatewayAddress(dhcp_offered_gw_add);
            setIpDnsAddress(dhcp_offered_dns_add);

            if (leaseT1 == 0)
            {
                leaseT1 = leaseSeconds / 2;
            }
            if (leaseT2 == 0)
            {
                leaseT2 = (leaseSeconds * 7) / 8;
            }

            // restart the expiration timers
            startOneshotTimer(callbackDhcpT1HitTimer, leaseT1);
            startOneshotTimer(callbackDhcpT2HitTimer, leaseT2);
            startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds);

            dhcpState = DHCP_BOUND;
        }
        else if (is_dhcp_nak(ether))
        {
            // the server refused to renew our lease
            // so we also have to return to init state
            init_state1();
        }
    }*/

    // we are rebinding and waiting for a reply from any server
    else if (dhcpState == DHCP_REBINDING)
    {
        if (isDhcpAck(ether))
        {
            // a server responded and extended our lease
            stopTimer(callbackDhcpT2PeriodicTimer);
            stopTimer(callbackDhcpLeaseEndTimer);

            handleDhcpAck(ether);

            setIpAddress(dhcpOfferedIpAdd);
            setIpSubnetMask(dhcp_offered_sn_mask);
            setIpGatewayAddress(dhcp_offered_gw_add);
            setIpDnsAddress(dhcp_offered_dns_add);

            if (leaseT1 == 0)
            {
                leaseT1 = leaseSeconds / 2;
            }
            if (leaseT2 == 0)
            {
                leaseT2 = (leaseSeconds * 7) / 8;
            }

            startOneshotTimer(callbackDhcpT1HitTimer, leaseT1);
            startOneshotTimer(callbackDhcpT2HitTimer, leaseT2);
            startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds);

            dhcpState = DHCP_BOUND;
        }
        else if (is_dhcp_nak(ether))
        {
            // our rebind request was rejected
            init_state1();
        }
    }
}

void processDhcpArpResponse(etherHeader *ether)
{
    // check if we are in the testing state and the flag is set
    if ( (dhcpState == DHCP_TESTING_IP) && (ipConflictDetectionMode) )
    {
        // someone replied to our arp probe
        // this means the ip is already in use
        ipConflictDetectionMode = false;
        stopTimer(callbackDhcpIpConflictWindow);

        // tell the dhcp server the ip is taken
        sendDhcpMessage(ether, DHCPDECLINE);

        // restart from scratch
        init_state1();
    }
}

// DHCP control functions

void enableDhcp()
{
    // to enable dhcp we set the only flag
    dhcpEnabled = true;
    init_state1();      // and then enter the init state
}

void disableDhcp()
{
    // to disable dhcp we have to clear all flags
    dhcpEnabled = false;
    discoverNeeded = false;
    requestNeeded = false;
    releaseNeeded = false;
    ipConflictDetectionMode = false;

    // then kill all of the timers and enter the disabled state
    kill_dhcp_timers();
    dhcpState = DHCP_DISABLED;
}

bool isDhcpEnabled()
{
    return dhcpEnabled;
}
