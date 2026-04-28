// TCP Library
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

// Names: Michael, Oscar, Subhaan

// in ethernet, every time we get a packet, we will check
// if it is a tcp packet with isTcp(), and then if it is, it calls
// processTcpResponse() which is what handles the state machine
// also every time in the main loop it also calls sendTcpPendingMessages()
// which is what retries the SYN if the connection messes up
// overall pretty similar in how dhcp is handled/starts state machine
// but state machine here is SYN, SYN-ACK, ACK, and we are established
// when we are established. mqtt sends messages as tcp payloads
// and leaving established there are 2 methods:
// if we disconnect: ESTABLISHED, FINWAIT1, FINWAIT2, TIME_WAIT, CLOSED
// if the broker disconnects: ESTABLISHED, CLOSE_WAIT, LAST_ACK, CLOSED

#include <stdio.h>
#include <string.h>
#include "arp.h"
#include "tcp.h"
#include "timer.h"
#include "mqtt.h"
#include "uart0.h"

//-----------------------------------------------------------------------------
//  Globals
//-----------------------------------------------------------------------------

#define MAX_TCP_PORTS 4
#define MAX_SOCKETS 10          // from socket
#define TCP_WINDOW_SIZE 1280    // tcp window size = 1 mss = 1280 from slides
#define MAX_PACKET_SIZE 1518    // from main

// if SYN msg fails then we resend after num of seconds
// and will do it up to the max num of retries
#define TCP_SYN_RETRY_SECONDS 3
#define TCP_SYN_MAX_RETRIES 5

// num of seconds we will wait in the TIME_WAIT state
#define TCP_TIME_WAIT_SECONDS 3

// server side variables, not used
uint16_t tcpPorts[MAX_TCP_PORTS];
uint8_t tcpPortCount = 0;
uint8_t tcpState[MAX_TCP_PORTS];


extern socket sockets[];    // from socket

// flags for SYN retries
bool tcpSynRetryExpired = false;
uint8_t tcpSynRetryCount = 0;

// flag for TIME_WAIT state
bool tcpTimeWaitExpired = false;

//-----------------------------------------------------------------------------
//  Structures
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void callbackTcpSynRetryTimer(void)
{
    // if the SYN retry interval expires
    // then we set the flag
    tcpSynRetryExpired = true;
}

void callbackTcpTimeWaitTimer(void)
{
    // if the TIME_WAIT interval expires
    // then set the flag
    tcpTimeWaitExpired = true;
}

uint16_t getTcpFlagsValue(tcpHeader *tcp)
{
    // from the offsetFields in tcp header we take out the (4 (data offset), 3 (reserved), 9 (flags))
    // 9 flag bits, but we only really need to use the bottom 5
    // ACK, PSH, RST, SYN, FIN
    return ntohs(tcp->offsetFields) & 0x1FF;
}

uint8_t getTcpHeaderLength(tcpHeader *tcp)
{
    // from the offsetFields in tcp header we take out the 4 data offset
    // bits by shifting right by 12, then multiply by 4 to convert to bytes
    // to get our TCP header length
    return (uint8_t)(((ntohs(tcp->offsetFields) >> OFS_SHIFT) & 0xF) * 4);
}

uint16_t getTcpSegmentLength(ipHeader *ip)
{
    // calculate full tcp packet length
    // full ip packet length - ip header size = ip payload = tcp packet
    return ntohs(ip->length) - (ip->size * 4);
}

uint16_t getTcpPayloadLength(ipHeader *ip, tcpHeader *tcp)
{
    // calculate tcp payload length by doing
    // full tcp packet length - tcp header length = tcp data payload
    return getTcpSegmentLength(ip) - getTcpHeaderLength(tcp);
}

// here we calculate how much the sequence number advances every packet
// sequence number tracks total bytes sent so we start with payload length
// syn and fin flags also each advance the sequence number by 1
uint32_t getTcpSequenceAdvance(tcpHeader *tcp, uint16_t payloadLength)
{
    uint16_t flags = getTcpFlagsValue(tcp);
    uint32_t advance = payloadLength;
    if ((flags & SYN) != 0)
        advance++;
    if ((flags & FIN) != 0)
        advance++;
    return advance;
}

bool ipAddressesMatch(const uint8_t a[4], const uint8_t b[4])
{
    // returns true if the ips are the same
    return memcmp(a, b, IP_ADD_LENGTH) == 0;
}

// checks if the mac address is valid if its not zeroes
// if it is all zeroes, then that means we need to send an arp req
bool macAddressValid(const uint8_t hw[6])
{
    uint8_t i;
    bool valid = false;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        valid = valid || (hw[i] != 0);
    return valid;
}

// matches the incoming packets to the right socket
// by checking which socket has the same ports and IP
// address as the incoming tcp packet
socket *findTcpSocket(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    uint8_t i;

    for (i = 0; i < MAX_SOCKETS; i++)
    {
        if (sockets[i].state != TCP_CLOSED &&
            sockets[i].localPort == ntohs(tcp->destPort) &&
            sockets[i].remotePort == ntohs(tcp->sourcePort) &&
            ipAddressesMatch(sockets[i].remoteIpAddress, ip->sourceIp))
        {
            return &sockets[i];
        }
    }
    return NULL;
}

// determine the next ip address that we need to talk to
void getTcpRouteIp(const uint8_t remoteIp[4], uint8_t routeIp[4])
{
    uint8_t i;
    bool sameSubnet = true;
    uint8_t localIp[4];
    uint8_t mask[4];
    uint8_t gateway[4];

    getIpAddress(localIp);
    getIpSubnetMask(mask);
    getIpGatewayAddress(gateway);

    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        // compares the local IP (src) and remote IP (dest) using
        // subnet mask to see if they are on same local network
        if ((localIp[i] & mask[i]) != (remoteIp[i] & mask[i]))
            sameSubnet = false;
    }

    // if they are on the same local network, then we can just
    // talk to the remote IP (original dest), but if not on same
    // subnet we have to talk to gateway ip address
    for (i = 0; i < IP_ADD_LENGTH; i++)
        routeIp[i] = sameSubnet ? remoteIp[i] : gateway[i];
}

// creates the entire TCP message we will send out, very similar to
// sendUdpMessage but separated into sendTcpSegment and sendTcpMessage
void sendTcpSegment(etherHeader *ether, socket *s, uint16_t flags, uint8_t data[], uint16_t dataSize)
{
    uint8_t i;
    uint16_t j;
    uint16_t ipHeaderLength;
    uint16_t tcpHeaderLength;
    uint16_t tcpLength;
    uint16_t tmp16;
    uint32_t sum;
    uint8_t localHwAddress[HW_ADD_LENGTH];
    uint8_t localIpAddress[IP_ADD_LENGTH];
    ipHeader *ip;
    tcpHeader *tcp;
    uint8_t *copyData;

    // header length with no options is 20 bytes / 4 = 5
    uint8_t tcpDataOffset = 5;

    // same as UDP
    getEtherMacAddress(localHwAddress); // getting our local mac address
    getIpAddress(localIpAddress);       // get our local ip address

    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        ether->destAddress[i] = s->remoteHwAddress[i];  // socket has mac address of who we are talking to
        ether->sourceAddress[i] = localHwAddress[i];    // our mac address we got from calling function
    }
    ether->frameType = htons(TYPE_IP);  // set the ethernet frame type for an IP packet

    // same as UDP
    ip = (ipHeader*)ether->data;
    ip->rev = 0x4;                  // set as ipv4 datagram
    ip->size = 0x5;                 // the size is 4*5 = 20 bytes with no options
    ip->typeOfService = 0;
    ip->id = 0;
    ip->flagsAndOffset = 0;
    ip->ttl = 128;                  // set time to live to 128 jumps
    ip->protocol = PROTOCOL_TCP;    // set ip protocol to TCP
    ip->headerChecksum = 0;
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        ip->destIp[i] = s->remoteIpAddress[i];  // get destination ip from socket
        ip->sourceIp[i] = localIpAddress[i];    // get local ip address
    }
    ipHeaderLength = ip->size * 4;

    // now we build the tcp header using all the info from socket
    tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    tcp->sourcePort = htons(s->localPort);
    tcp->destPort = htons(s->remotePort);
    tcp->sequenceNumber = htonl(s->sequenceNumber);
    tcp->acknowledgementNumber = htonl(s->acknowledgementNumber);
    tcp->windowSize = htons(TCP_WINDOW_SIZE);
    tcp->checksum = 0;
    tcp->urgentPointer = 0;

    // when we send the original SYN message we need to add
    // the MSS option and increase the header size
    if ((flags & SYN) != 0)
    {
        tcpDataOffset = 6;  // we add 4 bytes of options so 24 / 4 = 6
        tcp->data[0] = 2;   // first byte = kind = 2 for MSS
        tcp->data[1] = 4;   // second byte = total length = 4 bytes
        tcp->data[2] = (TCP_WINDOW_SIZE >> 8) & 0xFF;   // high byte of 1280 = 0x05
        tcp->data[3] = TCP_WINDOW_SIZE & 0xFF;          // low byte of 1280 = 0x00
    }

    // now we put the data offset in with the flags
    tcp->offsetFields = htons((tcpDataOffset << OFS_SHIFT) | flags);

    // now that we have filled out the header, we copy the data payload
    copyData = (uint8_t*)tcp + (tcpDataOffset * 4);
    for (j = 0; j < dataSize; j++)
        copyData[j] = data[j];

    // calculate all of the lenghts for the checksum
    tcpHeaderLength = tcpDataOffset * 4;
    tcpLength = tcpHeaderLength + dataSize;
    ip->length = htons(ipHeaderLength + tcpLength);
    calcIpChecksum(ip);

    // now compute the checksum same way as udp
    // over pseudoheader and entire packet
    sum = 0;
    sumIpWords(ip->sourceIp, 8, &sum);
    tmp16 = ip->protocol;
    sum += (tmp16 & 0xFF) << 8;
    tmp16 = htons(tcpLength);
    sumIpWords(&tmp16, 2, &sum);
    sumIpWords(tcp, tcpLength, &sum);
    tcp->checksum = getIpChecksum(sum);

    // now put the entire packet together
    putEtherPacket(ether, sizeof(etherHeader) + ipHeaderLength + tcpLength);

    // after we send the packet we increment the sequence number
    // syn and fin both increment it by 1, and then we increment
    // by total data size since SN tracks total bytes sent
    if ((flags & SYN) != 0)
        s->sequenceNumber++;
    if ((flags & FIN) != 0)
        s->sequenceNumber++;
    s->sequenceNumber += dataSize;
}

void sendTcpAckForPacket(etherHeader *ether, socket *s)
{
    // send an ACK message with the socket sent
    sendTcpResponse(ether, s, ACK);
}

// Set TCP state, server side
void setTcpState(uint8_t instance, uint8_t state)
{
    tcpState[instance] = state;
}

// Get TCP state, server side
uint8_t getTcpState(uint8_t instance)
{
    return tcpState[instance];
}

// Determines whether packet is TCP packet
// Must be an IP packet
bool isTcp(etherHeader* ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    tcpHeader *tcp;
    uint8_t ipHeaderLength;
    uint16_t tcpLength;
    uint16_t tmp16;
    uint32_t sum = 0;
    bool ok;

    // checks if the ip protocol is TCP
    ok = (ip->protocol == PROTOCOL_TCP);
    if (!ok)
        return false;

    // if it is TCP protocl we verify checksum
    // so calculate lengths here
    ipHeaderLength = ip->size * 4;
    tcpLength = ntohs(ip->length) - ipHeaderLength;
    tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);

    // perform actual tcp checksum over packet and pseudoheader
    sumIpWords(ip->sourceIp, 8, &sum);
    tmp16 = ip->protocol;
    sum += (tmp16 & 0xFF) << 8;
    tmp16 = htons(tcpLength);
    sumIpWords(&tmp16, 2, &sum);
    sumIpWords(tcp, tcpLength, &sum);
    return getIpChecksum(sum) == 0;
}

// checks if we got a SYN message
bool isTcpSyn(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + (ip->size * 4));
    uint16_t flags = getTcpFlagsValue(tcp);
    // checks if it is SYN and checks if there is NO ACK
    return ((flags & SYN) != 0) && ((flags & ACK) == 0);
}

// checks if we got an ACK message
bool isTcpAck(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + (ip->size * 4));
    return (getTcpFlagsValue(tcp) & ACK) != 0;  // checks if ACK flag is set
}

// this starts a new tcp connection using the correct socket
// and by sending the first SYN msg to server/broker
bool openTcpConnection(socket *s)
{
    uint8_t buffer[MAX_PACKET_SIZE];
    etherHeader *ether = (etherHeader*)buffer;
    uint8_t localIp[IP_ADD_LENGTH];
    uint8_t routeIp[IP_ADD_LENGTH];

    // if socket doesn't exist, exit
    if (s == NULL)
        return false;

    // if there is no local port set, set it to
    // be a random port in the dynamic range
    if (s->localPort == 0)
        s->localPort = 49152 + (random32() & 0x0FFF);

    // start with a random sequence number
    if (s->sequenceNumber == 0)
        s->sequenceNumber = random32();

    s->acknowledgementNumber = 0;   // no ACK so set to 0 b/c server hasn't sent anything yet
    s->state = TCP_SYN_SENT;        // now we update the socket state since we are sending SYN

    getIpAddress(localIp);
    getTcpRouteIp(s->remoteIpAddress, routeIp); // get the next ip we will talk to

    // if we have the mac address already then send the SYN msg
    if (macAddressValid(s->remoteHwAddress))
        sendTcpSegment(ether, s, SYN, NULL, 0);
    else
        sendArpRequest(ether, localIp, routeIp);    // otherwise send arp req to get mac address

    // now if the SYN failed, we will resend the SYN msg w/ timer
    tcpSynRetryCount = 0;
    tcpSynRetryExpired = false;
    stopTimer(callbackTcpSynRetryTimer);
    startOneshotTimer(callbackTcpSynRetryTimer, TCP_SYN_RETRY_SECONDS);

    return true;
}

// this closes the tcp connection by sending FIN-ACK
void closeTcpConnection(socket *s)
{
    uint8_t buffer[MAX_PACKET_SIZE];
    etherHeader *ether = (etherHeader*)buffer;

    if (s == NULL)
        return;

    // if we are still established then start closing it
    if (s->state == TCP_ESTABLISHED)
    {
        s->state = TCP_FIN_WAIT_1;  // move to FIN_WAIT_1
        sendTcpSegment(ether, s, ACK | FIN, NULL, 0);
    }
    else if (s->state == TCP_CLOSE_WAIT)    // if other side wants to close then start as well
    {
        s->state = TCP_LAST_ACK;    // move to LAST_ACK state
        sendTcpSegment(ether, s, ACK | FIN, NULL, 0);
    }
    else    // if we are in any other state just delete the socket
    {
        deleteSocket(s);
    }
}

bool isTcpEstablished(socket *s)
{
    // returns true if the tcp connection/state is established
    return (s != NULL) && (s->state == TCP_ESTABLISHED);
}

// called every time in main ethernet loop and handles
// the syn retry message if we are stuck in SYN_SENT state
// and also handles the TIME_WAIT state
void sendTcpPendingMessages(etherHeader *ether)
{
    // only send messages when the ethernet link is up
    if (isEtherLinkUp())
    {
        uint8_t i;

        // if the syn retry timer expired we need to resend it
        if (tcpSynRetryExpired)
        {
            tcpSynRetryExpired = false;

            for (i = 0; i < MAX_SOCKETS; i++)
            {
                // find the correct socket
                if (sockets[i].state == TCP_SYN_SENT)
                {
                    if (tcpSynRetryCount >= TCP_SYN_MAX_RETRIES)
                    {
                        // if we have reached the max retry limit
                        // then just close the socket and give up
                        putsUart0("\r\ntcp SYN retry limit reached\r\n");
                        stopTimer(callbackTcpSynRetryTimer);
                        mqttTcpClosed(&sockets[i]);
                        deleteSocket(&sockets[i]);
                    }
                    else
                    {
                        // we havent reached the limit so increment retry count
                        tcpSynRetryCount++;

                        if (macAddressValid(sockets[i].remoteHwAddress))
                        {
                            // if we have the mac address resend the SYN
                            // decrement sequence number cause the SYN failed before
                            sockets[i].sequenceNumber--;
                            sendTcpSegment(ether, &sockets[i], SYN, NULL, 0);
                        }
                        else
                        {
                            // if we dont have the mac address then send
                            // ARP request again to get it
                            uint8_t localIp[IP_ADD_LENGTH];
                            uint8_t routeIp[IP_ADD_LENGTH];
                            getIpAddress(localIp);
                            getTcpRouteIp(sockets[i].remoteIpAddress, routeIp);
                            sendArpRequest(ether, localIp, routeIp);
                        }

                        // restart the timer for next attempt
                        startOneshotTimer(callbackTcpSynRetryTimer, TCP_SYN_RETRY_SECONDS);
                    }
                }
            }
        }

        // if the TIME_WAIT timer expired we need to close the connection
        if (tcpTimeWaitExpired)
        {
            tcpTimeWaitExpired = false;

            for (i = 0; i < MAX_SOCKETS; i++)
            {
                // delete socket in TIME_WAIT state
                if (sockets[i].state == TCP_TIME_WAIT)
                {
                    deleteSocket(&sockets[i]);
                }
            }
        }
    }
}

// handle all incoming TCP packets, handles the state machine
void processTcpResponse(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    tcpHeader *tcp = (tcpHeader*) ((uint8_t*) ip + ipHeaderLength);
    socket *s = findTcpSocket(ether);
    uint16_t payloadLength = getTcpPayloadLength(ip, tcp);
    uint16_t flags = getTcpFlagsValue(tcp);
    uint32_t incomingSequence = ntohl(tcp->sequenceNumber);
    uint32_t incomingAck = ntohl(tcp->acknowledgementNumber);
    uint32_t sequenceAdvance = getTcpSequenceAdvance(tcp, payloadLength);
    uint8_t *payload = ((uint8_t*) tcp) + getTcpHeaderLength(tcp);

    if (s == NULL)
        return;

    // if the packet we received had the ACK flag set
    if ((flags & ACK) != 0)
    {
        // if we are closing and got ACK then move onto next FIN_WAIT state
        if (s->state == TCP_FIN_WAIT_1 && incomingAck == s->sequenceNumber)
            s->state = TCP_FIN_WAIT_2;

        // if we are closing and got ACK then completely close the connection
        if (s->state == TCP_LAST_ACK && incomingAck == s->sequenceNumber)
        {
            mqttTcpClosed(s);
            deleteSocket(s);
            return;
        }
    }

    // as a client if we have sent the SYN and are waiting for the SYN-ACK
    if (s->state == TCP_SYN_SENT)
    {
        if ((flags & (SYN | ACK)) == (SYN | ACK) && incomingAck == s->sequenceNumber)
        {
            // save the ack number as our ISN + 1 and move onto established state
            s->acknowledgementNumber = incomingSequence + 1;
            s->state = TCP_ESTABLISHED;
            putsUart0("\r\nTCP established\r\n");

            // stop the syn retry timer now that we are established
            stopTimer(callbackTcpSynRetryTimer);

            // send the final ACK for the 3 msg handshake and then notify mqtt
            sendTcpAckForPacket(ether, s);
            mqttTcpOpened(s);
        }
        else if ((flags & RST) != 0)
        {
            // if we got an RST flag sent to us we stop talking to the server
            // and close the "connection" (not connected yet)
            putsUart0("\r\nTCP connection refused (RST)\r\n");
            stopTimer(callbackTcpSynRetryTimer);
            mqttTcpClosed(s);
            deleteSocket(s);
        }
        return;
    }

    // now we handle the data and ensure that everything is
    // in order and we are not missing data
    if (payloadLength != 0)
    {
        if (incomingSequence == s->acknowledgementNumber)
        {
            s->acknowledgementNumber += payloadLength;
            mqttTcpDataReceived(s, payload, payloadLength);

            // only send ack if there isnt any FIN flag set
            // so that no double ACK msgs are sent
            if ((flags & FIN) == 0)
                sendTcpAckForPacket(ether, s);
        }
    }

    // now handle any incoming FIN messages and close the connection
    if ((flags & FIN) != 0)
    {
        // increment ack number so that we include all incoming data and the FIN
        s->acknowledgementNumber = incomingSequence + sequenceAdvance;
        sendTcpAckForPacket(ether, s);  // send ACK after we got the FIN

        if (s->state == TCP_FIN_WAIT_2 || s->state == TCP_FIN_WAIT_1)
        {
            putsUart0("\r\nTCP closing (TIME_WAIT)\r\n");
            // move onto TIME_WAIT state right before closing
            s->state = TCP_TIME_WAIT;
            mqttTcpClosed(s);           // tell mqtt TCP is closing
            tcpTimeWaitExpired = false;
            stopTimer(callbackTcpTimeWaitTimer);
            // start short timer before closing the socket completely
            startOneshotTimer(callbackTcpTimeWaitTimer, TCP_TIME_WAIT_SECONDS);
        }
        else
        {
            putsUart0("\r\nTCP FIN from broker (CLOSE_WAIT)\r\n");
            // the broker told us to close the connection so
            // move to CLOSE_WAIT state and send FIN-ACK
            s->state = TCP_CLOSE_WAIT;
            closeTcpConnection(s);
        }
        return;
    }

    // if we get an RST msg, then we stop talking to server
    if ((flags & RST) != 0)
    {
        putsUart0("\r\nTCP RST received\r\n");
        mqttTcpClosed(s);
        deleteSocket(s);
    }
}

// here we handle and arp replies when we are looking for
// a mac address, so we fill the socket once we get it
void processTcpArpResponse(etherHeader *ether)
{
    uint8_t routeIp[IP_ADD_LENGTH];
    uint8_t i;
    arpPacket *arp = (arpPacket*)ether->data;

    // look if any of the sockets are waiting for mac address
    for (i = 0; i < MAX_SOCKETS; i++)
    {
        // if there is a socket that sent a SYN without a valid mac address
        if (sockets[i].state == TCP_SYN_SENT && !macAddressValid(sockets[i].remoteHwAddress))
        {
            // get the ip that we need to talk to for the socket
            getTcpRouteIp(sockets[i].remoteIpAddress, routeIp);

            // if the arp response ip is the one we are looking for
            if (ipAddressesMatch(routeIp, arp->sourceIp))
            {
                // then save the mac address into the socket and send the SYN again
                memcpy(sockets[i].remoteHwAddress, arp->sourceAddress, HW_ADD_LENGTH);
                openTcpConnection(&sockets[i]);
            }
        }
    }
}

// server side function
void setTcpPortList(uint16_t ports[], uint8_t count)
{
}

bool isTcpPortOpen(etherHeader *ether)
{
    // return true if we have an active socket for this packet
    return (findTcpSocket(ether) != NULL);
}

// send a tcp response and if we dont have an open socket
// open it for the new connection
void sendTcpResponse(etherHeader *ether, socket* s, uint16_t flags)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    socket tempSocket;

    // if no socket
    if (s == NULL)
    {
        // create a socket for the incoming tcp packet
        memset(&tempSocket, 0, sizeof(tempSocket));
        getSocketInfoFromTcpPacket(ether, &tempSocket);
        tempSocket.sequenceNumber = 0;
        tempSocket.acknowledgementNumber =
            ntohl(tcp->sequenceNumber) + getTcpSequenceAdvance(tcp, getTcpPayloadLength(ip, tcp));
        s = &tempSocket;
    }

    // send the actual tcp response
    sendTcpSegment(ether, s, flags, NULL, 0);
}

// Send TCP message
void sendTcpMessage(etherHeader *ether, socket *s, uint16_t flags, uint8_t data[], uint16_t dataSize)
{
    // only send tcp message when the connection is established or in close wait state
    // called by mqtt to send the actual mqtt packets over tcp connection
    if ((s == NULL) || (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT))
        return;

    // send the actual packet with all of the data
    sendTcpSegment(ether, s, flags, data, dataSize);
}
