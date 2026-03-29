// MQTT Library (framework only)
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
#include "mqtt.h"
#include "timer.h"
#include "ip.h"
#include "socket.h"
#include "uart0.h"
#include "eth0.h"

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

#define MQTT_PORT 1883
#define MQTT_KEEPALIVE_SECONDS 60
#define MQTT_IDLE_SECONDS ((MQTT_KEEPALIVE_SECONDS > 1) ? (MQTT_KEEPALIVE_SECONDS / 2) : 1)
#define MQTT_PING_RESPONSE_TIMEOUT_SECONDS 10
#define MQTT_QOS0 0
#define MAX_MQTT_PACKET_SIZE 256
#define MAX_PACKET_SIZE 1518

socket *mqttSocket = NULL;
uint16_t mqttPacketId = 0;
bool mqttConnectPending = false;
bool mqttSessionEstablished = false;
bool mqttPingOutstanding = false;
bool mqttKeepaliveExpired = false;

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Local helpers
//-----------------------------------------------------------------------------

static uint16_t writeMqttString(uint8_t buffer[], uint16_t index, const char *str)
{
    uint16_t length = (uint16_t)strlen(str);
    buffer[index++] = (length >> 8) & 0xFF;
    buffer[index++] = length & 0xFF;
    memcpy(&buffer[index], str, length);
    return index + length;
}

static uint8_t encodeRemainingLength(uint16_t length, uint8_t encoded[])
{
    uint8_t count = 0;
    do
    {
        uint8_t byte = length % 128;
        length /= 128;
        if (length != 0)
            byte |= 0x80;
        encoded[count++] = byte;
    }
    while (length != 0);
    return count;
}

static bool decodeRemainingLength(uint8_t data[], uint16_t size, uint16_t *value, uint8_t *bytesUsed)
{
    uint16_t multiplier = 1;
    uint16_t total = 0;
    uint8_t index = 1;
    uint8_t encodedByte;

    do
    {
        if (index >= size)
            return false;
        encodedByte = data[index++];
        total += (encodedByte & 0x7F) * multiplier;
        multiplier *= 128;
    }
    while ((encodedByte & 0x80) != 0 && index < 5);

    *value = total;
    *bytesUsed = index - 1;
    return true;
}

static void callbackMqttKeepaliveTimer(void)
{
    mqttKeepaliveExpired = true;
}

static void armMqttKeepaliveTimer(uint32_t seconds)
{
    if (!restartTimer(callbackMqttKeepaliveTimer))
        startOneshotTimer(callbackMqttKeepaliveTimer, seconds);
}

static void sendMqttPacket(uint8_t controlType, uint8_t flags, uint8_t payload[], uint16_t payloadSize)
{
    uint8_t buffer[MAX_MQTT_PACKET_SIZE];
    uint8_t header[4];
    uint8_t headerSize;
    uint16_t packetSize;
    uint8_t etherBuffer[MAX_PACKET_SIZE];
    etherHeader *ether = (etherHeader*)etherBuffer;

    if ((mqttSocket == NULL) || !isTcpEstablished(mqttSocket))
        return;
    if (controlType != 1 && !mqttSessionEstablished)
        return;

    buffer[0] = (controlType << 4) | (flags & 0x0F);
    headerSize = encodeRemainingLength(payloadSize, header);
    if ((1 + headerSize + payloadSize) > MAX_MQTT_PACKET_SIZE)
        return;
    memcpy(&buffer[1], header, headerSize);
    if (payloadSize != 0)
        memcpy(&buffer[1 + headerSize], payload, payloadSize);
    packetSize = 1 + headerSize + payloadSize;

    sendTcpMessage(ether, mqttSocket, ACK | PSH, buffer, packetSize);

    if (controlType == 12)
    {
        mqttPingOutstanding = true;
        armMqttKeepaliveTimer(MQTT_PING_RESPONSE_TIMEOUT_SECONDS);
    }
    else if (mqttSessionEstablished && controlType != 14)
    {
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
    }
}

static void resetMqttState(void)
{
    stopTimer(callbackMqttKeepaliveTimer);
    mqttConnectPending = false;
    mqttSessionEstablished = false;
    mqttPingOutstanding = false;
    mqttKeepaliveExpired = false;
    mqttSocket = NULL;
}

static uint16_t nextPacketId(void)
{
    if (++mqttPacketId == 0)
        mqttPacketId = 1;
    return mqttPacketId;
}

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void connectMqtt()
{
    uint8_t ip[IP_ADD_LENGTH];

    if (mqttSocket != NULL && mqttSocket->state != TCP_CLOSED)
        return;

    getIpMqttBrokerAddress(ip);
    if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0)
        return;

    mqttSocket = newSocket();
    if (mqttSocket == NULL)
        return;

    memset(mqttSocket, 0, sizeof(socket));
    memcpy(mqttSocket->remoteIpAddress, ip, IP_ADD_LENGTH);
    mqttSocket->remotePort = MQTT_PORT;
    mqttSocket->localPort = 51000 + (random32() & 0x03FF);
    mqttSocket->state = TCP_CLOSED;
    mqttConnectPending = true;
    mqttSessionEstablished = false;
    mqttPingOutstanding = false;
    mqttKeepaliveExpired = false;
    openTcpConnection(mqttSocket);
}

void disconnectMqtt()
{
    if (mqttSocket == NULL)
        return;

    if (mqttSessionEstablished && isTcpEstablished(mqttSocket))
        sendMqttPacket(14, 0, NULL, 0);

    mqttSessionEstablished = false;
    closeTcpConnection(mqttSocket);
    if (mqttSocket->state == TCP_CLOSED)
        resetMqttState();
}

void publishMqtt(char strTopic[], char strData[])
{
    uint8_t payload[MAX_MQTT_PACKET_SIZE];
    uint16_t index = 0;

    index = writeMqttString(payload, index, strTopic);
    memcpy(&payload[index], strData, strlen(strData));
    index += strlen(strData);

    sendMqttPacket(3, MQTT_QOS0, payload, index);
}

void subscribeMqtt(char strTopic[])
{
    uint8_t payload[MAX_MQTT_PACKET_SIZE];
    uint16_t packetId;
    uint16_t index = 0;

    packetId = nextPacketId();
    payload[index++] = (packetId >> 8) & 0xFF;
    payload[index++] = packetId & 0xFF;
    index = writeMqttString(payload, index, strTopic);
    payload[index++] = 0;

    sendMqttPacket(8, 2, payload, index);
}

void unsubscribeMqtt(char strTopic[])
{
    uint8_t payload[MAX_MQTT_PACKET_SIZE];
    uint16_t packetId;
    uint16_t index = 0;

    packetId = nextPacketId();
    payload[index++] = (packetId >> 8) & 0xFF;
    payload[index++] = packetId & 0xFF;
    index = writeMqttString(payload, index, strTopic);

    sendMqttPacket(10, 2, payload, index);
}

void serviceMqtt()
{
    if ((mqttSocket == NULL) || !mqttSessionEstablished || !isTcpEstablished(mqttSocket))
        return;
    if (!mqttKeepaliveExpired)
        return;

    mqttKeepaliveExpired = false;

    if (mqttPingOutstanding)
    {
        putsUart0("MQTT keepalive timeout\n");
        disconnectMqtt();
    }
    else
    {
        sendMqttPacket(12, 0, NULL, 0);
    }
}

void mqttTcpOpened(socket *s)
{
    uint8_t payload[MAX_MQTT_PACKET_SIZE];
    uint8_t mac[HW_ADD_LENGTH];
    char clientId[20];
    uint16_t index = 0;

    if (s != mqttSocket || !mqttConnectPending)
        return;

    getEtherMacAddress(mac);
    snprintf(clientId, sizeof(clientId), "tm4c-%02u%02u%02u", mac[3], mac[4], mac[5]);

    index = writeMqttString(payload, index, "MQTT");
    payload[index++] = 4;
    payload[index++] = 2;
    payload[index++] = (MQTT_KEEPALIVE_SECONDS >> 8) & 0xFF;
    payload[index++] = MQTT_KEEPALIVE_SECONDS & 0xFF;
    index = writeMqttString(payload, index, clientId);

    mqttConnectPending = false;
    sendMqttPacket(1, 0, payload, index);
}

void mqttTcpClosed(socket *s)
{
    if (s != mqttSocket)
        return;

    resetMqttState();
}

void mqttTcpDataReceived(socket *s, uint8_t data[], uint16_t size)
{
    uint8_t packetType;
    uint16_t index;
    uint16_t remainingLength;
    uint16_t topicLength;
    uint8_t remainingLengthBytes;
    char topic[64];
    char message[96];

    if (s != mqttSocket || size == 0)
        return;

    if (!decodeRemainingLength(data, size, &remainingLength, &remainingLengthBytes))
        return;

    packetType = data[0] >> 4;
    index = 1 + remainingLengthBytes;
    if (packetType == 2 && remainingLength >= 2 && (index + 1) < size)
    {
        mqttSessionEstablished = (data[index + 1] == 0);
        if (mqttSessionEstablished)
        {
            mqttPingOutstanding = false;
            armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
            putsUart0("MQTT connected\n");
        }
        else
            putsUart0("MQTT connack error\n");
    }
    else if (packetType == 13)
    {
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
    }
    else if (packetType == 3 && remainingLength >= 2 && index < size)
    {
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
        topicLength = (data[index] << 8) | data[index + 1];
        index += 2;
        if (topicLength >= sizeof(topic) || (index + topicLength) > size)
            return;

        memcpy(topic, &data[index], topicLength);
        topic[topicLength] = 0;
        index += topicLength;

        if ((data[0] & 0x06) != 0)
            index += 2;
        if (index >= size)
            return;

        if ((size - index) >= sizeof(message))
            return;

        memcpy(message, &data[index], size - index);
        message[size - index] = 0;
        putsUart0("MQTT ");
        putsUart0(topic);
        putsUart0(": ");
        putsUart0(message);
        putsUart0("\n");
    }
    else if (mqttSessionEstablished)
    {
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
    }
}
