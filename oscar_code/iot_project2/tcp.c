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

#include <stdio.h>
#include <string.h>
#include "arp.h"
#include "tcp.h"
#include "timer.h"
#include "mqtt.h"

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

#define MAX_TCP_PORTS 4
#define MAX_SOCKETS 10
#define TCP_WINDOW_SIZE 1460
#define MAX_PACKET_SIZE 1518

uint16_t tcpPorts[MAX_TCP_PORTS];
uint8_t tcpPortCount = 0;
uint8_t tcpState[MAX_TCP_PORTS];
extern socket sockets[];

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Local helpers
//-----------------------------------------------------------------------------

static uint16_t getTcpFlagsValue(tcpHeader *tcp)
{
    return ntohs(tcp->offsetFields) & 0x01FF;
}

static uint8_t getTcpHeaderLength(tcpHeader *tcp)
{
    return (uint8_t)(((ntohs(tcp->offsetFields) >> OFS_SHIFT) & 0x0F) * 4);
}

static uint16_t getTcpSegmentLength(ipHeader *ip)
{
    return ntohs(ip->length) - (ip->size * 4);
}

static uint16_t getTcpPayloadLength(ipHeader *ip, tcpHeader *tcp)
{
    return getTcpSegmentLength(ip) - getTcpHeaderLength(tcp);
}

static uint32_t getTcpSequenceAdvance(tcpHeader *tcp, uint16_t payloadLength)
{
    uint16_t flags = getTcpFlagsValue(tcp);
    uint32_t advance = payloadLength;
    if ((flags & SYN) != 0)
        advance++;
    if ((flags & FIN) != 0)
        advance++;
    return advance;
}

static bool ipAddressesMatch(const uint8_t a[4], const uint8_t b[4])
{
    return memcmp(a, b, IP_ADD_LENGTH) == 0;
}

static bool macAddressValid(const uint8_t hw[6])
{
    uint8_t i;
    bool valid = false;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        valid = valid || (hw[i] != 0);
    return valid;
}

static socket *findTcpSocket(etherHeader *ether)
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

static bool isLocalPortOpen(uint16_t port)
{
    uint8_t i;
    for (i = 0; i < tcpPortCount; i++)
    {
        if (tcpPorts[i] == port)
            return true;
    }
    return false;
}

static void getTcpRouteIp(const uint8_t remoteIp[4], uint8_t routeIp[4])
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
        if ((localIp[i] & mask[i]) != (remoteIp[i] & mask[i]))
            sameSubnet = false;
    }

    for (i = 0; i < IP_ADD_LENGTH; i++)
        routeIp[i] = sameSubnet ? remoteIp[i] : gateway[i];
}

static void sendTcpSegment(etherHeader *ether, socket *s, uint16_t flags, uint8_t data[], uint16_t dataSize)
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

    getEtherMacAddress(localHwAddress);
    getIpAddress(localIpAddress);

    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        ether->destAddress[i] = s->remoteHwAddress[i];
        ether->sourceAddress[i] = localHwAddress[i];
    }
    ether->frameType = htons(TYPE_IP);

    ip = (ipHeader*)ether->data;
    ip->rev = 0x4;
    ip->size = 0x5;
    ip->typeOfService = 0;
    ip->id = 0;
    ip->flagsAndOffset = 0;
    ip->ttl = 128;
    ip->protocol = PROTOCOL_TCP;
    ip->headerChecksum = 0;
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        ip->destIp[i] = s->remoteIpAddress[i];
        ip->sourceIp[i] = localIpAddress[i];
    }
    ipHeaderLength = ip->size * 4;

    tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    tcp->sourcePort = htons(s->localPort);
    tcp->destPort = htons(s->remotePort);
    tcp->sequenceNumber = htonl(s->sequenceNumber);
    tcp->acknowledgementNumber = htonl(s->acknowledgementNumber);
    tcp->offsetFields = htons((5 << OFS_SHIFT) | flags);
    tcp->windowSize = htons(TCP_WINDOW_SIZE);
    tcp->checksum = 0;
    tcp->urgentPointer = 0;

    copyData = tcp->data;
    for (j = 0; j < dataSize; j++)
        copyData[j] = data[j];

    tcpHeaderLength = getTcpHeaderLength(tcp);
    tcpLength = tcpHeaderLength + dataSize;
    ip->length = htons(ipHeaderLength + tcpLength);
    calcIpChecksum(ip);

    sum = 0;
    sumIpWords(ip->sourceIp, 8, &sum);
    tmp16 = ip->protocol;
    sum += (tmp16 & 0xFF) << 8;
    tmp16 = htons(tcpLength);
    sumIpWords(&tmp16, 2, &sum);
    sumIpWords(tcp, tcpLength, &sum);
    tcp->checksum = getIpChecksum(sum);

    putEtherPacket(ether, sizeof(etherHeader) + ipHeaderLength + tcpLength);

    if ((flags & SYN) != 0)
        s->sequenceNumber++;
    if ((flags & FIN) != 0)
        s->sequenceNumber++;
    s->sequenceNumber += dataSize;
}

static void sendTcpAckForPacket(etherHeader *ether, socket *s)
{
    sendTcpResponse(ether, s, ACK);
}

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// Set TCP state
void setTcpState(uint8_t instance, uint8_t state)
{
    tcpState[instance] = state;
}

// Get TCP state
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

    ok = (ip->protocol == PROTOCOL_TCP);
    if (!ok)
        return false;

    ipHeaderLength = ip->size * 4;
    tcpLength = ntohs(ip->length) - ipHeaderLength;
    tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);

    sumIpWords(ip->sourceIp, 8, &sum);
    tmp16 = ip->protocol;
    sum += (tmp16 & 0xFF) << 8;
    tmp16 = htons(tcpLength);
    sumIpWords(&tmp16, 2, &sum);
    sumIpWords(tcp, tcpLength, &sum);
    return getIpChecksum(sum) == 0;
}

bool isTcpSyn(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + (ip->size * 4));
    uint16_t flags = getTcpFlagsValue(tcp);
    return ((flags & SYN) != 0) && ((flags & ACK) == 0);
}

bool isTcpAck(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + (ip->size * 4));
    return (getTcpFlagsValue(tcp) & ACK) != 0;
}

bool openTcpConnection(socket *s)
{
    uint8_t buffer[MAX_PACKET_SIZE];
    etherHeader *ether = (etherHeader*)buffer;
    uint8_t localIp[IP_ADD_LENGTH];
    uint8_t routeIp[IP_ADD_LENGTH];

    if (s == NULL)
        return false;

    if (s->localPort == 0)
        s->localPort = 49152 + (random32() & 0x0FFF);
    if (s->sequenceNumber == 0)
        s->sequenceNumber = random32();
    s->acknowledgementNumber = 0;
    s->state = TCP_SYN_SENT;

    getIpAddress(localIp);
    getTcpRouteIp(s->remoteIpAddress, routeIp);

    if (macAddressValid(s->remoteHwAddress))
        sendTcpSegment(ether, s, SYN, NULL, 0);
    else
        sendArpRequest(ether, localIp, routeIp);

    return true;
}

void closeTcpConnection(socket *s)
{
    uint8_t buffer[MAX_PACKET_SIZE];
    etherHeader *ether = (etherHeader*)buffer;

    if (s == NULL)
        return;

    if (s->state == TCP_ESTABLISHED)
    {
        s->state = TCP_FIN_WAIT_1;
        sendTcpSegment(ether, s, ACK | FIN, NULL, 0);
    }
    else if (s->state == TCP_CLOSE_WAIT)
    {
        s->state = TCP_LAST_ACK;
        sendTcpSegment(ether, s, ACK | FIN, NULL, 0);
    }
    else
    {
        deleteSocket(s);
    }
}

bool isTcpEstablished(socket *s)
{
    return (s != NULL) && (s->state == TCP_ESTABLISHED);
}

void sendTcpPendingMessages(etherHeader *ether)
{
    (void)ether;
}

void processTcpResponse(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    socket *s = findTcpSocket(ether);
    uint16_t payloadLength = getTcpPayloadLength(ip, tcp);
    uint16_t flags = getTcpFlagsValue(tcp);
    uint32_t incomingSequence = ntohl(tcp->sequenceNumber);
    uint32_t incomingAck = ntohl(tcp->acknowledgementNumber);
    uint32_t sequenceAdvance = getTcpSequenceAdvance(tcp, payloadLength);
    uint8_t *payload = ((uint8_t*)tcp) + getTcpHeaderLength(tcp);

    if (s == NULL)
    {
        if (isLocalPortOpen(ntohs(tcp->destPort)) && isTcpSyn(ether))
        {
            s = newSocket();
            if (s != NULL)
            {
                memset(s, 0, sizeof(socket));
                getSocketInfoFromTcpPacket(ether, s);
                s->sequenceNumber = random32();
                s->acknowledgementNumber = incomingSequence + 1;
                s->state = TCP_SYN_RECEIVED;
                sendTcpResponse(ether, s, SYN | ACK);
            }
        }
        return;
    }

    if ((flags & ACK) != 0)
    {
        if (s->state == TCP_SYN_RECEIVED && incomingAck == s->sequenceNumber)
            s->state = TCP_ESTABLISHED;
        if (s->state == TCP_FIN_WAIT_1 && incomingAck == s->sequenceNumber)
            s->state = TCP_FIN_WAIT_2;
        if (s->state == TCP_LAST_ACK && incomingAck == s->sequenceNumber)
        {
            mqttTcpClosed(s);
            deleteSocket(s);
            return;
        }
    }

    if (s->state == TCP_SYN_SENT)
    {
        if ((flags & (SYN | ACK)) == (SYN | ACK) && incomingAck == s->sequenceNumber)
        {
            s->acknowledgementNumber = incomingSequence + 1;
            s->state = TCP_ESTABLISHED;
            sendTcpAckForPacket(ether, s);
            mqttTcpOpened(s);
        }
        else if ((flags & RST) != 0)
        {
            mqttTcpClosed(s);
            deleteSocket(s);
        }
        return;
    }

    if (payloadLength != 0)
    {
        if (incomingSequence == s->acknowledgementNumber)
        {
            s->acknowledgementNumber += payloadLength;
            mqttTcpDataReceived(s, payload, payloadLength);
            sendTcpAckForPacket(ether, s);
        }
    }

    if ((flags & FIN) != 0)
    {
        s->acknowledgementNumber = incomingSequence + sequenceAdvance;
        sendTcpAckForPacket(ether, s);

        if (s->state == TCP_FIN_WAIT_2 || s->state == TCP_FIN_WAIT_1)
        {
            mqttTcpClosed(s);
            deleteSocket(s);
        }
        else
        {
            s->state = TCP_CLOSE_WAIT;
            closeTcpConnection(s);
        }
        return;
    }

    if ((flags & RST) != 0)
    {
        mqttTcpClosed(s);
        deleteSocket(s);
    }
}

void processTcpArpResponse(etherHeader *ether)
{
    uint8_t routeIp[IP_ADD_LENGTH];
    uint8_t i;
    arpPacket *arp = (arpPacket*)ether->data;

    for (i = 0; i < MAX_SOCKETS; i++)
    {
        if (sockets[i].state == TCP_SYN_SENT && !macAddressValid(sockets[i].remoteHwAddress))
        {
            getTcpRouteIp(sockets[i].remoteIpAddress, routeIp);
            if (ipAddressesMatch(routeIp, arp->sourceIp))
            {
                memcpy(sockets[i].remoteHwAddress, arp->sourceAddress, HW_ADD_LENGTH);
                openTcpConnection(&sockets[i]);
            }
        }
    }
}

void setTcpPortList(uint16_t ports[], uint8_t count)
{
    uint8_t i;
    tcpPortCount = (count > MAX_TCP_PORTS) ? MAX_TCP_PORTS : count;
    for (i = 0; i < tcpPortCount; i++)
    {
        tcpPorts[i] = ports[i];
        tcpState[i] = TCP_LISTEN;
    }
    for (; i < MAX_TCP_PORTS; i++)
    {
        tcpPorts[i] = 0;
        tcpState[i] = TCP_CLOSED;
    }
}

bool isTcpPortOpen(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    uint16_t localPort = ntohs(tcp->destPort);

    return (findTcpSocket(ether) != NULL) || isLocalPortOpen(localPort);
}

void sendTcpResponse(etherHeader *ether, socket* s, uint16_t flags)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    tcpHeader *tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    socket tempSocket;

    if (s == NULL)
    {
        memset(&tempSocket, 0, sizeof(tempSocket));
        getSocketInfoFromTcpPacket(ether, &tempSocket);
        tempSocket.sequenceNumber = 0;
        tempSocket.acknowledgementNumber =
            ntohl(tcp->sequenceNumber) + getTcpSequenceAdvance(tcp, getTcpPayloadLength(ip, tcp));
        s = &tempSocket;
    }

    sendTcpSegment(ether, s, flags, NULL, 0);
}

// Send TCP message
void sendTcpMessage(etherHeader *ether, socket *s, uint16_t flags, uint8_t data[], uint16_t dataSize)
{
    if ((s == NULL) || (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT))
        return;

    sendTcpSegment(ether, s, flags, data, dataSize);
}
