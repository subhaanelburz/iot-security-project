// Ethernet Framework for DHCP- and MQTT-based Projects
// Spring 2026
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: EK-TM4C123GXL w/ ENC28J60
// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

// Hardware configuration:
// ENC28J60 Ethernet controller on SPI0
//   MOSI (SSI0Tx) on PA5
//   MISO (SSI0Rx) on PA4
//   SCLK (SSI0Clk) on PA2
//   ~CS (SW controlled) on PA3
//   WOL on PB3
//   INT on PC6

// Pinning for IoT projects with wireless modules:
// N24L01+ RF transceiver
//   MOSI (SSI0Tx) on PA5
//   MISO (SSI0Rx) on PA4
//   SCLK (SSI0Clk) on PA2
//   ~CS on PE0
//   INT on PB2
// Xbee module
//   DIN (UART1TX) on PC5
//   DOUT (UART1RX) on PC4

//-----------------------------------------------------------------------------
// Configuring Wireshark to examine packets
//-----------------------------------------------------------------------------

// sudo ethtool --offload eno2 tx off rx off
// in wireshark, preferences->protocol->ipv4->validate the checksum if possible
// in wireshark, preferences->protocol->udp->validate the checksum if possible

//-----------------------------------------------------------------------------
// Sending UDP test packets
//-----------------------------------------------------------------------------

// test this with a udp send utility like sendip
//   send ipv4 (-p ipv4) udp protocol packet (-p udp)
//      with contents (-d) "on" or "off"
//   from 192.168.1.x (-is), port 10000 (-us)
//   to   192.168.1.y, port 1024 (-ud)
// sendip -p ipv4 -is 192.168.1.x -p udp -us 10000 -ud 1024 -d "on" 192.168.1.y
// sendip -p ipv4 -is 192.168.1.x -p udp -us 10000 -ud 1024 -d "off" 192.168.1.y

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "tm4c123gh6pm.h"
#include "clock.h"
#include "eeprom.h"
#include "gpio.h"
#include "spi0.h"
#include "uart0.h"
#include "wait.h"
#include "timer.h"
#include "eth0.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "dhcp.h"
#include "mqtt.h"
#include "nfc.h"

// Pins
#define RED_LED PORTF,1
#define BLUE_LED PORTF,2
#define GREEN_LED PORTF,3
#define PUSH_BUTTON PORTF,4

// EEPROM Map
#define EEPROM_DHCP        1
#define EEPROM_IP          2
#define EEPROM_SUBNET_MASK 3
#define EEPROM_GATEWAY     4
#define EEPROM_DNS         5
#define EEPROM_TIME        6
#define EEPROM_MQTT        7
#define EEPROM_ERASED      0xFFFFFFFF

#define NFC_MQTT_TOPIC     "tm4c/nfc"

//-----------------------------------------------------------------------------
// Subroutines                
//-----------------------------------------------------------------------------

// Initialize Hardware
void initHw()
{
    // Initialize system clock to 40 MHz
    initSystemClockTo40Mhz();

    // Enable clocks
    enablePort(PORTF);
    _delay_cycles(3);

    // Configure LED and pushbutton pins
    selectPinPushPullOutput(RED_LED);
    selectPinPushPullOutput(GREEN_LED);
    selectPinPushPullOutput(BLUE_LED);
    selectPinDigitalInput(PUSH_BUTTON);
    enablePinPullup(PUSH_BUTTON);
}

void displayConnectionInfo()
{
    uint8_t i;
    char str[20];
    uint8_t mac[6];
    uint8_t ip[4];
    getEtherMacAddress(mac);
    putsUart0("  HW:    ");
    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%02"PRIu8, mac[i]);
        putsUart0(str);
        if (i < HW_ADD_LENGTH-1)
            putcUart0(':');
    }
    putcUart0('\n');
    getIpAddress(ip);
    putsUart0("  IP:    ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%"PRIu8, ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    if (isDhcpEnabled())
        putsUart0(" (dhcp)");
    else
        putsUart0(" (static)");
    putcUart0('\n');
    getIpSubnetMask(ip);
    putsUart0("  SN:    ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%"PRIu8, ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    getIpGatewayAddress(ip);
    putsUart0("  GW:    ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%"PRIu8, ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    getIpDnsAddress(ip);
    putsUart0("  DNS:   ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%"PRIu8, ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    getIpTimeServerAddress(ip);
    putsUart0("  Time:  ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%"PRIu8, ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    getIpMqttBrokerAddress(ip);
    putsUart0("  MQTT:  ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%"PRIu8, ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    if (isDhcpEnabled())
    {
        putsUart0("  Lease: ");
        uint32_t s, m, h, d;
        s = getDhcpLeaseSeconds();
        d = s / (24*60*60);
        s -= d * (24*60*60);
        h = s / (60*60);
        s -= h * (60*60);
        m = s / 60;
        snprintf(str, sizeof(str), "%"PRIu32"d:%02"PRIu32"h:%02"PRIu32"m\n", d, h, m);
        putsUart0(str);
    }
    if (isEtherLinkUp())
        putsUart0("  Link is up\n");
    else
        putsUart0("  Link is down\n");
}

void displayNfcUid(uint8_t *uid, uint8_t uidLength)
{
    uint8_t i;
    char str[8];

    putsUart0("NFC UID: ");
    for (i = 0; i < uidLength; i++)
    {
        snprintf(str, sizeof(str), "%02X", uid[i]);
        putsUart0(str);
        if (i < uidLength - 1)
            putcUart0(':');
    }
    putcUart0('\n');
}

void formatNfcUid(uint8_t *uid, uint8_t uidLength, char *str, uint8_t maxChars)
{
    uint8_t i;
    uint8_t index = 0;

    if (maxChars == 0)
        return;

    for (i = 0; i < uidLength && (index + 2) < maxChars; i++)
    {
        index += snprintf(&str[index], maxChars - index, "%02X", uid[i]);
        if ((i < (uidLength - 1)) && ((index + 1) < maxChars))
            str[index++] = ':';
    }
    str[index] = '\0';
}

char* getNfcName(uint8_t *uid, uint8_t uidLength)
{
    static const uint8_t oscarUid[] = {0x86, 0xB8, 0x2B, 0x07};
    static const uint8_t michealUid[] = {0x1D, 0xD0, 0x70, 0x0C, 0xA7, 0x00, 0x00};
    static const uint8_t subhaanUid[] = {0x1D, 0xFB, 0x32, 0x0C, 0xA7, 0x00, 0x00};

    if ((uidLength == sizeof(oscarUid)) && (memcmp(uid, oscarUid, sizeof(oscarUid)) == 0))
        return "oscar";
    if ((uidLength == sizeof(michealUid)) && (memcmp(uid, michealUid, sizeof(michealUid)) == 0))
        return "micheal";
    if ((uidLength == sizeof(subhaanUid)) && (memcmp(uid, subhaanUid, sizeof(subhaanUid)) == 0))
        return "subhaan";
    return "unknown";
}

void queueNfcMqttPublish(uint8_t *uid, uint8_t uidLength, char *topic, uint16_t topicChars, char *payload, uint16_t payloadChars)
{
    char uidString[3 * NFC_MAX_UID_LENGTH];
    char *name;

    formatNfcUid(uid, uidLength, uidString, sizeof(uidString));
    name = getNfcName(uid, uidLength);
    snprintf(topic, topicChars, "%s", NFC_MQTT_TOPIC);
    snprintf(payload, payloadChars, "{\"name\":\"%s\",\"uid\":\"%s\"}", name, uidString);
}

void readConfiguration()
{
    uint32_t temp;
    uint8_t* ip;

    if (readEeprom(EEPROM_DHCP) == EEPROM_ERASED)
    {
        enableDhcp();
    }
    else
    {
        disableDhcp();
        temp = readEeprom(EEPROM_IP);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpAddress(ip);
        }
        temp = readEeprom(EEPROM_SUBNET_MASK);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpSubnetMask(ip);
        }
        temp = readEeprom(EEPROM_GATEWAY);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpGatewayAddress(ip);
        }
        temp = readEeprom(EEPROM_DNS);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpDnsAddress(ip);
        }
        temp = readEeprom(EEPROM_TIME);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpTimeServerAddress(ip);
        }
        temp = readEeprom(EEPROM_MQTT);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpMqttBrokerAddress(ip);
        }
    }
}

#define MAX_CHARS 80
char strInput[MAX_CHARS+1];
char* token;
uint8_t count = 0;

uint8_t asciiToUint8(const char str[])
{
    uint8_t data;
    if (str[0] == '0' && tolower(str[1]) == 'x')
        sscanf(str, "%hhx", &data);
    else
        sscanf(str, "%hhu", &data);
    return data;
}

void processShell()
{
    bool end;
    char c;
    uint8_t i;
    uint8_t ip[IP_ADD_LENGTH];
    uint8_t nfcUid[NFC_MAX_UID_LENGTH];
    uint8_t nfcUidLength;
    uint32_t* p32;
    char *topic, *data;

    if (kbhitUart0())
    {
        c = getcUart0();

        end = (c == 13) || (count == MAX_CHARS);
        if (!end)
        {
            if ((c == 8 || c == 127) && count > 0)
                count--;
            if (c >= ' ' && c < 127)
                strInput[count++] = c;
        }
        else
        {
            strInput[count] = '\0';
            count = 0;
            token = strtok(strInput, " ");
            if (strcmp(token, "dhcp") == 0)
            {
                token = strtok(NULL, " ");
                if (strcmp(token, "renew") == 0)
                {
                    renewDhcp();
                }
                else if (strcmp(token, "release") == 0)
                {
                    releaseDhcp();
                }
                else if (strcmp(token, "on") == 0)
                {
                    enableDhcp();
                    writeEeprom(EEPROM_DHCP, EEPROM_ERASED);
                }
                else if (strcmp(token, "off") == 0)
                {
                    disableDhcp();
                    writeEeprom(EEPROM_DHCP, 0);
                }
                else
                    putsUart0("Error in dhcp argument\r");
            }
            if (strcmp(token, "mqtt") == 0)
            {
                token = strtok(NULL, " ");
                if (strcmp(token, "connect") == 0)
                {
                    connectMqtt();
                }
                if (strcmp(token, "disconnect") == 0)
                {
                    disconnectMqtt();
                }
                if (strcmp(token, "publish") == 0)
                {
                    topic = strtok(NULL, " ");
                    data = strtok(NULL, " ");
                    if (topic != NULL && data != NULL)
                        publishMqtt(topic, data);
                }
                if (strcmp(token, "subscribe") == 0)
                {
                    topic = strtok(NULL, " ");
                    if (topic != NULL)
                        subscribeMqtt(topic);
                }
                if (strcmp(token, "unsubscribe") == 0)
                {
                    topic = strtok(NULL, " ");
                    if (topic != NULL)
                        unsubscribeMqtt(topic);
                }
            }
            if (strcmp(token, "ip") == 0)
            {
                displayConnectionInfo();
            }
            if (strcmp(token, "ping") == 0)
            {
                for (i = 0; i < IP_ADD_LENGTH; i++)
                {
                    token = strtok(NULL, " .");
                    ip[i] = asciiToUint8(token);
                }
                //removed from this version to save space: sendPingRequest(ip)
            }
            if (strcmp(token, "reboot") == 0)
            {
                NVIC_APINT_R = NVIC_APINT_VECTKEY | NVIC_APINT_SYSRESETREQ;
            }
            if (strcmp(token, "set") == 0)
            {
                token = strtok(NULL, " ");
                if (strcmp(token, "ip") == 0)
                {
                    for (i = 0; i < IP_ADD_LENGTH; i++)
                    {
                        token = strtok(NULL, " .");
                        ip[i] = asciiToUint8(token);
                    }
                    setIpAddress(ip);
                    p32 = (uint32_t*)ip;
                    writeEeprom(EEPROM_IP, *p32);
                }
                if (strcmp(token, "sn") == 0)
                {
                    for (i = 0; i < IP_ADD_LENGTH; i++)
                    {
                        token = strtok(NULL, " .");
                        ip[i] = asciiToUint8(token);
                    }
                    setIpSubnetMask(ip);
                    p32 = (uint32_t*)ip;
                    writeEeprom(EEPROM_SUBNET_MASK, *p32);
                }
                if (strcmp(token, "gw") == 0)
                {
                    for (i = 0; i < IP_ADD_LENGTH; i++)
                    {
                        token = strtok(NULL, " .");
                        ip[i] = asciiToUint8(token);
                    }
                    setIpGatewayAddress(ip);
                    p32 = (uint32_t*)ip;
                    writeEeprom(EEPROM_GATEWAY, *p32);
                }
                if (strcmp(token, "dns") == 0)
                {
                    for (i = 0; i < IP_ADD_LENGTH; i++)
                    {
                        token = strtok(NULL, " .");
                        ip[i] = asciiToUint8(token);
                    }
                    setIpDnsAddress(ip);
                    p32 = (uint32_t*)ip;
                    writeEeprom(EEPROM_DNS, *p32);
                }
                if (strcmp(token, "time") == 0)
                {
                    for (i = 0; i < IP_ADD_LENGTH; i++)
                    {
                        token = strtok(NULL, " .");
                        ip[i] = asciiToUint8(token);
                    }
                    setIpTimeServerAddress(ip);
                    p32 = (uint32_t*)ip;
                    writeEeprom(EEPROM_TIME, *p32);
                }
                if (strcmp(token, "mqtt") == 0)
                {
                    for (i = 0; i < IP_ADD_LENGTH; i++)
                    {
                        token = strtok(NULL, " .");
                        ip[i] = asciiToUint8(token);
                    }
                    setIpMqttBrokerAddress(ip);
                    p32 = (uint32_t*)ip;
                    writeEeprom(EEPROM_MQTT, *p32);
                }
            }

            if (strcmp(token, "help") == 0)
            {
                putsUart0("Commands:\n");
                putsUart0("  dhcp on|off|renew|release\n");
                putsUart0("  mqtt ACTION [USER [PASSWORD]]\n");
                putsUart0("    where ACTION = {connect|disconnect|publish TOPIC DATA\n");
                putsUart0("                   |subscribe TOPIC|unsubscribe TOPIC}\n");
                putsUart0("  nfc\n");
                putsUart0("  ip\n");
                putsUart0("  ping w.x.y.z\n");
                putsUart0("  reboot\n");
                putsUart0("  set ip|gw|dns|time|mqtt|sn w.x.y.z\n");
            }
            if (strcmp(token, "nfc") == 0)
            {
                nfcUidLength = 0;
                if (nfcReadUid(nfcUid, &nfcUidLength))
                    displayNfcUid(nfcUid, nfcUidLength);
                else
                    putsUart0("No NFC tag detected\n");
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Main
//-----------------------------------------------------------------------------

// Max packet is calculated as:
// Ether frame header (18) + Max MTU (1500)
#define MAX_PACKET_SIZE 1518

int main(void)
{
    uint8_t* udpData;
    uint8_t buffer[MAX_PACKET_SIZE];
    uint8_t nfcUid[NFC_MAX_UID_LENGTH];
    uint8_t lastNfcUid[NFC_MAX_UID_LENGTH];
    uint8_t nfcUidLength;
    uint8_t lastNfcUidLength;
    uint16_t nfcPollDivider;
    char nfcMqttTopic[32];
    char nfcMqttPayload[96];
    etherHeader *data = (etherHeader*) buffer;
    socket s;
    bool nfcPresent;
    bool nfcPublishPending;

    // Init controller
    initHw();

    // Setup UART0
    initUart0();
    setUart0BaudRate(115200, 40e6);

    // Init timer
    initTimer();

    // Init PN532 NFC reader on I2C1
    putsUart0("Starting NFC\n");
    if (initNfc())
        putsUart0("NFC ready\n");
    else
        putsUart0("NFC init failed\n");

    // Init sockets
    initSockets();

    // Init ethernet interface (eth0)
    putsUart0("\nStarting eth0\n");
    initEther(ETHER_UNICAST | ETHER_BROADCAST | ETHER_HALFDUPLEX);
    setEtherMacAddress(2, 3, 4, 5, 6, 111); // changed the x to 111

    // Init EEPROM
    initEeprom();
    readConfiguration();

    setPinValue(GREEN_LED, 1);
    waitMicrosecond(100000);
    setPinValue(GREEN_LED, 0);
    waitMicrosecond(100000);

    lastNfcUidLength = 0;
    nfcPresent = false;
    nfcPollDivider = 0;
    nfcPublishPending = false;

    // Main Loop
    // RTOS and interrupts would greatly improve this code,
    // but the goal here is simplicity
    while (true)
    {
        // Put terminal processing here
        processShell();

        // DHCP maintenance
        if (isDhcpEnabled())
        {
            sendDhcpPendingMessages(data);
        }

        // TCP pending messages
        sendTcpPendingMessages(data);

        // MQTT keepalive / ping handling
        serviceMqtt();

        // Publish pending NFC scan once MQTT is connected
        if (nfcPublishPending)
        {
            if (!isMqttConnected())
                connectMqtt();
            else
            {
                publishMqtt(nfcMqttTopic, nfcMqttPayload);
                putsUart0("MQTT publish ");
                putsUart0(nfcMqttPayload);
                putsUart0("\n");
                nfcPublishPending = false;
            }
        }

        // NFC tag polling
        // Poll less aggressively so UART shell stays responsive.
        if (!kbhitUart0() && (++nfcPollDivider >= 100))
        {
            nfcPollDivider = 0;
            nfcUidLength = 0;
            if (nfcReadUid(nfcUid, &nfcUidLength))
            {
                if (!nfcPresent || (nfcUidLength != lastNfcUidLength) || (memcmp(nfcUid, lastNfcUid, nfcUidLength) != 0))
                {
                    memcpy(lastNfcUid, nfcUid, nfcUidLength);
                    lastNfcUidLength = nfcUidLength;
                    nfcPresent = true;
                    displayNfcUid(nfcUid, nfcUidLength);
                    queueNfcMqttPublish(nfcUid, nfcUidLength, nfcMqttTopic, sizeof(nfcMqttTopic), nfcMqttPayload, sizeof(nfcMqttPayload));
                    nfcPublishPending = true;
                    setPinValue(BLUE_LED, 1);
                    waitMicrosecond(50000);
                    setPinValue(BLUE_LED, 0);
                }
            }
            else
            {
                nfcPresent = false;
                lastNfcUidLength = 0;
            }
        }

        // Packet processing
        if (isEtherDataAvailable())
        {
            if (isEtherOverflow())
            {
                setPinValue(RED_LED, 1);
                waitMicrosecond(100000);
                setPinValue(RED_LED, 0);
            }

            // Get packet
            getEtherPacket(data, MAX_PACKET_SIZE);

            // Handle ARP request
            if (isArpRequest(data))
                sendArpResponse(data);

            // Route ARP response to appropriate handlers
            // DHCP uses ARP response to verify address granted is not in use
            // TCP active open uses ARP response to get the HW address to establish the socket
            if (isArpResponse(data))
            {
                processDhcpArpResponse(data);
                processTcpArpResponse(data);
            }

            // Handle IP datagram
            if (isIp(data))
            {
            	if (isIpUnicast(data))
            	{
                    // Handle ICMP ping request
                    if (isPingRequest(data))
                    {
                        sendPingResponse(data);
                    }

                    // Handle TCP datagram
                    if (isTcp(data))
                    {
                        if (isTcpPortOpen(data))
                        {
                            processTcpResponse(data);
                        }
                        else
                            sendTcpResponse(data, NULL, ACK | RST);
                    }
                }
            	// Handle DHCP response
                if (isUdp(data))
                {
                    if (isDhcpResponse(data))
                        processDhcpResponse(data);
                    else
                    {
                    // Handle UDP messages on any port, so feel free to comment out when working on DHCP
                    udpData = getUdpData(data);
                    if (strcmp((char*)udpData, "on") == 0)
                        setPinValue(GREEN_LED, 1);
                    if (strcmp((char*)udpData, "off") == 0)
                        setPinValue(GREEN_LED, 0);
                    getSocketInfoFromUdpPacket(data, &s);
                    sendUdpMessage(data, s, (uint8_t*)"Received", 9);
                    }
                }
            }
        }
    }
}

