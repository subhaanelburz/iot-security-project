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

// Names: Michael, Oscar, Subhaan

// mqtt is the tcp data payload we use for the project
// it will communicate all our sensor information to the broker
// which will send to all subscribers, after in established state in
// tcp, mqttTcpOpened() gets called and we send the CONNECT msg, then
// once the broker responds with CONNACK, that means we are connected
// and we can subscribe, publish, etc.
// in ethernet loop, we call serviceMqtt() to handle the keep alive timer
// and we make sure to send a ping request before the broker disconnects us

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

#define MQTT_PORT 1883              // default mqtt port number (unencrypted)
#define MQTT_KEEPALIVE_SECONDS 60   // how long before we get disconnected by the broker (if no messages sent)
#define MQTT_IDLE_SECONDS 30        // how long client waits before sending ping request (so doesn't get disconnected by broker)

// how long to wait for ping response after sending ping request
// if we get the ping response, then we reset all timers
#define MQTT_PING_RESPONSE_TIMEOUT_SECONDS 10

#define MQTT_QOS0 0                 // use qos0 = send once, no guarantee of delivery
#define MAX_MQTT_PACKET_SIZE 256    // max size of mqtt packet
#define MAX_PACKET_SIZE 1518        // from main

// mqtt packet types from slides
#define MQTT_CONNECT        1
#define MQTT_CONNACK        2
#define MQTT_PUBLISH        3
// packet types 4, 5, 6, and 7 are all for qos1/2 which were not implementing
#define MQTT_SUBSCRIBE      8
#define MQTT_SUBACK         9
#define MQTT_UNSUBSCRIBE    10
#define MQTT_UNSUBACK       11
#define MQTT_PINGREQ        12
#define MQTT_PINGRESP       13
#define MQTT_DISCONNECT     14

socket *mqttSocket = NULL;              // the tcp socket we will open for mqtt port
uint16_t mqttPacketId = 0;              // packet id for subscribe/unsubscribe packets
bool mqttConnectPending = false;        // flag true when tcp open but mqtt still pending
bool mqttSessionEstablished = false;    // flag true when we enter established state w/ last connack
bool mqttPingOutstanding = false;       // flag true when we send a ping request and are waiting for response
bool mqttKeepaliveExpired = false;      // flag true when the keep alive timer has expired

// declared in sensors handle incoming publish messages for nfc add/delete
extern void handleMqttPublish(const char *topic, const char *message);

//------------------------------------------------------------------------------
//  Structures
//------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// used for writing variable length headers like for publish
// or for any other mqtt string like subscribe payload
// all mqtt strings start with 2 byte length prefix then actual string
// NULL TERMINATORS NOT IMPLEMENTED*
uint16_t writeMqttString(uint8_t buffer[], uint16_t index, const char *str)
{
    uint16_t length = (uint16_t)strlen(str);    // get the string length
    buffer[index++] = (length >> 8) & 0xFF;     // MSB of length (switch to network order/big endian)
    buffer[index++] = length & 0xFF;            // LSB of length
    memcpy(&buffer[index], str, length);        // actual data/topic name, ex: distance
    return index + length;                      // return new index/size where data ends
}

// in mqtt packet header, here we encode the actual packet remaining length
// this is what prof drew on board of 7 bits storing actual value of length
// 0-127: 1 byte, 128-16,383: 2 bytes, 2^14-2^21-1: 3 bytes, 2^21-2^28-1: 4 bytes
uint8_t encodeRemainingLength(uint16_t length, uint8_t encoded[])
{
    uint8_t count = 0;
    do
    {
        uint8_t byte = length % 128;    // extract the 7 bits of actual length
        length /= 128;                  // reduce by 7 to get next 7 bits
        if (length != 0)                // if we still have a greater length
            byte |= 0x80;               // then we encode the 8th bit to = 1; same thing as board
        encoded[count++] = byte;        // then we write the encoded byte and increment count
    }
    while (length != 0);                // loop until we encode the entire length
    return count;                       // return the number of bytes we wrote to encode the length
}

// this function decodes the encoded remaining length field from
// any mqtt packet that we receive, start at index 1 to skip control header
bool decodeRemainingLength(uint8_t data[], uint16_t size, uint16_t *value, uint8_t *bytesUsed)
{
    uint16_t multiplier = 1;    // multiplier to decode the 7 bits so 1, 128, 128^2, etc.
    uint16_t total = 0;         // total calculated length
    uint8_t index = 1;          // start at remaining length field
    uint8_t encodedByte;

    do
    {
        if (index >= size)  // if the data exceeds limit return false
            return false;
        encodedByte = data[index++];                // get the current byte data and increment to next
        total += (encodedByte & 0x7F) * multiplier; // add the value to the total
        multiplier *= 128;                          // moving to next place so shift multiplier
    }
    while ((encodedByte & 0x80) != 0 && index < 5); // keep summing length if 8th bit is still set

    *value = total;             // update final length
    *bytesUsed = index - 1;     // how many bytes remaining length field was
    return true;
}

// handler/timer callback that runs when the keep alive timer expires
void callbackMqttKeepaliveTimer(void)
{
    mqttKeepaliveExpired = true;
}

// starts/restarts the keep alive timer, also used for ping/idle
// basically the timer will wait for 30s idle, then ping (wait 10s for response)
// if we dont get ping response then flag is still set and we disconnect
void armMqttKeepaliveTimer(uint32_t seconds)
{
    // use restart here since we keep resetting the duration of timer
    if (!restartTimer(callbackMqttKeepaliveTimer))
        startOneshotTimer(callbackMqttKeepaliveTimer, seconds);
}

// function where we actually build the mqtt packet over tcp header
// the mqtt packet has 3 main parts:
// the control header (1 byte): 4b for packet type, 4b for flags
// the packet remaining length (1-4 bytes)
// variable length header (>=0 bytes)
// payload (>= 0 bytes)
void sendMqttPacket(uint8_t controlType, uint8_t flags, uint8_t payload[], uint16_t payloadSize)
{
    uint8_t buffer[MAX_MQTT_PACKET_SIZE];
    uint8_t header[4];
    uint8_t headerSize;
    uint16_t packetSize;
    uint8_t etherBuffer[MAX_PACKET_SIZE];
    etherHeader *ether = (etherHeader*)etherBuffer;

    // if tcp isnt established, we cant send an mqtt packet so return
    if ((mqttSocket == NULL) || !isTcpEstablished(mqttSocket))
        return;

    // only allow connect packets to be sent when the connection isnt
    // established because we do not need to send connect after were connected
    if (controlType != MQTT_CONNECT && !mqttSessionEstablished)
        return;

    // now we build the control header by putting the
    // packet type into the upper 4 bits and the flags
    // into the lower 4 bits
    buffer[0] = (controlType << 4) | (flags & 0x0F);

    // here we encode the remaining packet length using helper function
    headerSize = encodeRemainingLength(payloadSize, header);

    // if we exceed the packet size then return cause we cant transmit it
    if ((1 + headerSize + payloadSize) > MAX_MQTT_PACKET_SIZE)
        return;

    memcpy(&buffer[1], header, headerSize); // actually copy the remaining length bytes

    // if we have a payload, then copy the payload as well
    if (payloadSize != 0)
        memcpy(&buffer[1 + headerSize], payload, payloadSize);

    // sum up the entire packet size so it can be sent
    packetSize = 1 + headerSize + payloadSize;

    //send packet with ACK (got prev data) and PSH (for data)
    sendTcpMessage(ether, mqttSocket, ACK | PSH, buffer, packetSize);

    // if the packet type we are sending is a ping request
    if (controlType == MQTT_PINGREQ)
    {
        // set the flag saying we are waiting for ping response
        mqttPingOutstanding = true;
        // restart keep alive timer to run for 10s to wait if we get ping response
        armMqttKeepaliveTimer(MQTT_PING_RESPONSE_TIMEOUT_SECONDS);
    }
    // if we are established and are not sending a disconnect packet
    else if (mqttSessionEstablished && controlType != MQTT_DISCONNECT)
    {
        // set the flag saying we are not waiting for ping response
        mqttPingOutstanding = false;
        // restart keep alive timer to run for 30s to idle
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
    }
}

// call to clear all mqtt flags, sockets, timers
void resetMqttState(void)
{
    stopTimer(callbackMqttKeepaliveTimer);
    mqttConnectPending = false;
    mqttSessionEstablished = false;
    mqttPingOutstanding = false;
    mqttKeepaliveExpired = false;
    mqttSocket = NULL;
}

// call every time for next subscribe/unsubscribe packet
// to increment the packet id; packet id has to be nonzero
uint16_t nextPacketId(void)
{
    if (++mqttPacketId == 0)    // increment packet id
        mqttPacketId = 1;       // if the packet id overflows to 0, reset it back to 1
    return mqttPacketId;
}

// function actually starts the mqtt connection process
// it will allocate the socket, fill in the info, and open tcp
// then once tcp is established, mqttTcpOpened() gets called from tcp
void connectMqtt()
{
    uint8_t ip[IP_ADD_LENGTH];

    // if we are already connected, then return
    if (mqttSocket != NULL && mqttSocket->state != TCP_CLOSED)
        return;

    // get the broker ip address that was set statically w/
    // "set mqtt 192.168.1.?"
    // ignore comment under function, it gets mqtt not time server
    getIpMqttBrokerAddress(ip);
    if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0)
        return;

    // create a new socket for the connection
    mqttSocket = newSocket();
    if (mqttSocket == NULL)
        return;

    // now we fill in all of the connection info for the socket
    // first we clear the socket memory (set to CLOSED state)
    memset(mqttSocket, 0, sizeof(socket));

    memcpy(mqttSocket->remoteIpAddress, ip, IP_ADD_LENGTH); // then set mqtt broker ip (destination)
    mqttSocket->remotePort = MQTT_PORT;                     // set port to MQTT 1883

    // set the flags, only connect pending true
    mqttConnectPending = true;
    mqttSessionEstablished = false;
    mqttPingOutstanding = false;
    mqttKeepaliveExpired = false;

    // now we call the function to actually start the TCP connection (sends first SYN)
    // SYN, SYN-ACK, ACK then connection is established
    // the CONNECT and CONNACK messages are done once mqttTcpOpened()
    // gets called in tcp after sending the last ACK
    openTcpConnection(mqttSocket);
}

// send a disconnect packet and close the tcp connection
void disconnectMqtt()
{
    // if there is no connection/socket, return
    if (mqttSocket == NULL)
        return;

    // if there is a connection established, then send a disconnect packet
    if (mqttSessionEstablished && isTcpEstablished(mqttSocket))
        sendMqttPacket(MQTT_DISCONNECT, 0, NULL, 0);

    // set the flags, close the connection
    mqttSessionEstablished = false;
    closeTcpConnection(mqttSocket);
    if (mqttSocket->state == TCP_CLOSED)    // ensure we are closed by calling reset
        resetMqttState();
}

bool isMqttConnected()
{
    // returns true if mqtt is connected
    // checks that the socket exists, the flag is tue, and if tcp is established
    return (mqttSocket != NULL) && mqttSessionEstablished && isTcpEstablished(mqttSocket);
}

// publish a message as a topic to mqtt
// publish packet has control header (4b packet type, 4b flags)
// packet remaining length (1-4 bytes), variable length header
// and the actual payload
// here we mainly do variable length + payload
void publishMqtt(char strTopic[], char strData[])
{
    uint8_t payload[MAX_MQTT_PACKET_SIZE];
    uint16_t index = 0;

    // first thing we do is build the variable length header
    // function calls and puts the 2 byte length and the topic string
    index = writeMqttString(payload, index, strTopic);

    // after that we make the actual payload of whatever data we are publishing
    // and that is just put into the buffer after variable length header
    memcpy(&payload[index], strData, strlen(strData));
    index += strlen(strData);   // add size of data payload to index

    // send the actual packet with packet type, QOS0 flags, payload and size
    // the function handles encoding the remaining length
    sendMqttPacket(MQTT_PUBLISH, MQTT_QOS0, payload, index);
}

// subscribe to any topic to receive sensor data
// packet has control header, remaining length, variable length
// header (packet id), data payload, and the qos
void subscribeMqtt(char strTopic[])
{
    uint8_t payload[MAX_MQTT_PACKET_SIZE];
    uint16_t packetId;
    uint16_t index = 0;

    // first thing we do it get the next packet id since we inc by 1 each time
    packetId = nextPacketId();

    // then we actually build the variable length header w/ MSB first
    payload[index++] = (packetId >> 8) & 0xFF;  // high byte
    payload[index++] = packetId & 0xFF;         // low byte

    // after we add the actual data payload to encode w/ 2 byte length
    index = writeMqttString(payload, index, strTopic);

    // add the qos0 at the end of the packet
    payload[index++] = 0;

    // send off actual packet, mqtt docs says flags = 0010 otherwise
    // the server treads it as malformed and closes the connection
    sendMqttPacket(MQTT_SUBSCRIBE, 2, payload, index);
}

// unsubscribe from a topic
// function is pretty much same as subscribe
// but doesnt require sending qos
void unsubscribeMqtt(char strTopic[])
{
    uint8_t payload[MAX_MQTT_PACKET_SIZE];
    uint16_t packetId;
    uint16_t index = 0;

    // first thing we do it get the next packet id since we inc by 1 each time
    packetId = nextPacketId();

    // then we actually build the variable length header w/ MSB first
    payload[index++] = (packetId >> 8) & 0xFF;  // high byte
    payload[index++] = packetId & 0xFF;         // low byte

    // after we add the actual data payload to encode w/ 2 byte length
    index = writeMqttString(payload, index, strTopic);

    // send packet, mqtt docs req flags = 0010
    sendMqttPacket(MQTT_UNSUBSCRIBE, 2, payload, index);
}

// call every time in ethernet loop to handle keep alive for connection
void serviceMqtt()
{
    // ensure mqtt connection still active and keep alive has expired
    if ((mqttSocket == NULL) || !mqttSessionEstablished || !isTcpEstablished(mqttSocket))
        return;
    if (!mqttKeepaliveExpired)
        return;

    mqttKeepaliveExpired = false;

    // if we already sent a ping and the broker never replied
    // then disconnect mqtt since no ping response
    if (mqttPingOutstanding)
    {
        putsUart0("\r\nno ping response. MQTT disconnecting.\r\n");
        disconnectMqtt();
    }
    else
    {
        // otherwise send the ping request to tell the broker to keep connection
        sendMqttPacket(MQTT_PINGREQ, 0, NULL, 0);
    }
}

// after tcp finishes the 3 way handshake
// it will call this and here we send the CONNECT packet
void mqttTcpOpened(socket *s)
{
    uint8_t payload[MAX_MQTT_PACKET_SIZE];
    uint8_t mac[HW_ADD_LENGTH];
    char clientId[20];
    uint16_t index = 0;

    // ensure we got the right socket and we need to connect to mqtt
    if (s != mqttSocket || !mqttConnectPending)
        return;

    // set client id to pid as specified by broker
    getEtherMacAddress(mac);
    snprintf(clientId, sizeof(clientId), "pid");

    // create the variable header for MQTT
    index = writeMqttString(payload, index, "MQTT");
    payload[index++] = 4;   // add the protocol level, 4 = mqtt 3.1.1
    payload[index++] = 2;   // connect specific flages, 2 = clean session meaning no user/pass/will
    payload[index++] = (MQTT_KEEPALIVE_SECONDS >> 8) & 0xFF;    // then keep alive timer inputed MSB first
    payload[index++] = MQTT_KEEPALIVE_SECONDS & 0xFF;           // LSB

    // only thing in our payload is the clientID that we made previously
    index = writeMqttString(payload, index, clientId);

    // clear flag and actually send the packet
    mqttConnectPending = false;
    sendMqttPacket(MQTT_CONNECT, 0, payload, index);
}

// called from tcp when it closes
void mqttTcpClosed(socket *s)
{
    // check if correct socket, if it is, reset mqtt
    if (s != mqttSocket)
        return;

    resetMqttState();
}

// called from when we get data from broker
// here we handle and parse through all of those incoming packets
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

    // for every packet that we get, decode the remaining length
    // to see where the payload starts and how many bytes tor read
    if (!decodeRemainingLength(data, size, &remainingLength, &remainingLengthBytes))
        return;

    // shift the first byte by 4 to get the packet type (skip flags)
    packetType = data[0] >> 4;
    index = 1 + remainingLengthBytes;   // move index past the header

    // handle any CONNACK msgs, when the broker replies to our connect packet
    if (packetType == MQTT_CONNACK && remainingLength >= 2 && (index + 1) < size)
    {
        // check the second byte for connect return code; 0 means connection is a success
        mqttSessionEstablished = (data[index + 1] == 0);
        if (mqttSessionEstablished)
        {
            // we are now connected so clear flag and start timer
            mqttPingOutstanding = false;
            armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
            putsUart0("\r\nMQTT successfully connected to broker\r\n");
        }
        else
            putsUart0("\r\nMQTT broker refused our CONNECT\r\n");
    }
    // handle PINGRESP msgs, when broker replies to our keepalive PINGREQ
    else if (packetType == MQTT_PINGRESP)
    {
        // since we got a response, clear flag and restart timer to idle duration
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
    }
    // handle PUBLISH msgs, when someone posts data to something we subscribed to
    else if (packetType == MQTT_PUBLISH && remainingLength >= 2 && index < size)
    {
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);

        // grab MSB and LSB of topic length
        // first index has MSB so shift it left 8
        // then move to next index to grab LSB
        // OR together to put into variable and get actual topic length
        topicLength = (data[index] << 8) | data[index + 1];
        index += 2; // move 2 bytes forward past the topic name length

        // if the topic is longer than what we can hold then return
        if (topicLength >= sizeof(topic) || (index + topicLength) > size)
            return;

        // copy the actual string of the topic
        memcpy(topic, &data[index], topicLength);
        topic[topicLength] = 0; // add null terminator to end of topic string
        index += topicLength;   // move past the the topic string to payload

        // if we got a packet > qos0 then move past packet id
        if ((data[0] & 0x06) != 0)
            index += 2;

        if (index >= size)
            return;
        if ((size - index) >= sizeof(message))
            return;

        // now copy the actual message payload
        memcpy(message, &data[index], size - index);
        message[size - index] = 0;  // add null terminator

        putsUart0("\r\nreceived MQTT data-");
        putsUart0(topic);
        putsUart0(": ");
        putsUart0(message);
        putsUart0("\r\n");

        // if we receive any publish messages for nfc add/delete handle it
        handleMqttPublish(topic, message);
    }
    // handle SUBACK msgs, broker confirms our subscription
    else if (packetType == MQTT_SUBACK && remainingLength >= 3 && (index + 2) < size)
    {
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);

        // skip past variable length header and check return code
        uint8_t returnCode = data[index + 2];
        if (returnCode == 0x80)                 // return code 0x80 means failure
            putsUart0("\r\nfailed to subscribe to MQTT topic\r\n");
        else
            putsUart0("\r\nsuccessfully subscribed to MQTT topic\r\n");
    }
    // handle UNSUBACK msgs, broker confirmed that we unsubscribed
    else if (packetType == MQTT_UNSUBACK && remainingLength >= 2)
    {
        // nothing to check so just clear flag and restart timer
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
        putsUart0("\r\nsuccessfully unsubscribed to MQTT topic\r\n");
    }
    // handle any other packets we might receive
    else if (mqttSessionEstablished)
    {
        // just clear flag and restart timer
        mqttPingOutstanding = false;
        armMqttKeepaliveTimer(MQTT_IDLE_SECONDS);
    }
}
