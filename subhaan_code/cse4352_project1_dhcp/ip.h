// IP Library
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

#ifndef IP_H_
#define IP_H_

#include <stdint.h>
#include <stdbool.h>
#include "eth0.h"

// prof mentioned pragma stuff is to fix C compiler since
// they technically are not to C standard?
// it forces the compiler to not add any padding basically
// this is just the standard ipv4 header
// the header is at data[0] inside the _etherHeader in memory
#pragma pack(push)
#pragma pack(1)
typedef struct _ipHeader // 20 or more bytes
{
    uint8_t size:4; // this is the upper 4 bits of the first byte
    uint8_t rev:4;  // this is the lower 4 bits of the first byte
    uint8_t typeOfService;
    uint16_t length;
    uint16_t id;    // id for each packet
    uint16_t flagsAndOffset;
    uint8_t ttl;    // time to live for the packet
    uint8_t protocol;   // 1 = ICMP, 6 = TCP, 17 = UDP
    uint16_t headerChecksum;    // this is the ones complement checksum for the header
    uint8_t sourceIp[4];    // source ip address
    uint8_t destIp[4];      // destination ip address
    uint8_t data[0]; // optional bytes or udp/tcp/icmp header
} ipHeader;
#pragma pack(pop)

// Protocols
#define PROTOCOL_ICMP 1
#define PROTOCOL_TCP  6
#define PROTOCOL_UDP  17

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

bool isIp(etherHeader *ether);
bool isIpUnicast(etherHeader *ether);
bool isIpValid();

void setIpAddress(const uint8_t ip[4]);
void getIpAddress(uint8_t ip[4]);
void setIpSubnetMask(const uint8_t mask[4]);
void getIpSubnetMask(uint8_t mask[4]);
void setIpGatewayAddress(const uint8_t ip[4]);
void getIpGatewayAddress(uint8_t ip[4]);
void setIpDnsAddress(const uint8_t ip[4]);
void getIpDnsAddress(uint8_t ip[4]);
void setIpTimeServerAddress(const uint8_t ip[4]);
void getIpTimeServerAddress(uint8_t ip[4]);
void setIpMqttBrokerAddress(const uint8_t ip[4]);
void getIpMqttBrokerAddress(uint8_t ip[4]);

void sumIpWords(void* data, uint16_t sizeInBytes, uint32_t* sum);
void calcIpChecksum(ipHeader* ip);
uint16_t getIpChecksum(uint32_t sum);

#endif

