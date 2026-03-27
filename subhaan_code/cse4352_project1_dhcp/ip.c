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

#include <stdio.h>
#include "ip.h"

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

// these arrays will hold all of the network settings for the tm4c
// they are set to 0.0.0.0 to default, and they will be overwritten
// statically from eeprom, or when dhcp gets a lease
uint8_t ipAddress[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipSubnetMask[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipGwAddress[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipDnsAddress[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipTimeServerAddress[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipMqttBrokerAddress[IP_ADD_LENGTH] = {0,0,0,0};

// ------------------------------------------------------------------------------
//  Structures
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// Determines whether packet is IP datagram
bool isIp(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;  // typecast ether data[0] since that is the ip header
    uint8_t ipHeaderLength = ip->size * 4;  // get the total size in bytes
    uint32_t sum = 0;
    bool ok;
    ok = (ether->frameType == htons(TYPE_IP));  // check if its an IP packet
    if (ok)
    {
        sumIpWords(ip, ipHeaderLength, &sum);   // calculate the sum of header
        ok = (getIpChecksum(sum) == 0);         // then check if the checksum is correct
    }
    return ok;
}

// Determines whether packet is unicast to this ip
// Must be an IP packet
bool isIpUnicast(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t i = 0;
    bool ok = true;
    getIpAddress(ipAddress);    // this will get the latest ip address we have been given
    while (ok && (i < IP_ADD_LENGTH))   // basically just checking eaach byte
    {
        ok = (ip->destIp[i] == ipAddress[i]);   // to see if the dest ip is the tm4c ip
        i++;
    }
    return ok;
}

// Determines if the IP address is valid
bool isEtherIpValid()
{
    // returns true if we have non zero ip address (meaning dhcp or eeprom worked)
    return ipAddress[0] || ipAddress[1] || ipAddress[2] || ipAddress[3];
}

// everything below just updates/sets the network settings

// Sets IP address
void setIpAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipAddress[i] = ip[i];
}

// Gets IP address
void getIpAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipAddress[i];
}

// Sets IP subnet mask
void setIpSubnetMask(const uint8_t mask[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipSubnetMask[i] = mask[i];
}

// Gets IP subnet mask
void getIpSubnetMask(uint8_t mask[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        mask[i] = ipSubnetMask[i];
}

// Sets IP gateway address
void setIpGatewayAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipGwAddress[i] = ip[i];
}

// Gets IP gateway address
void getIpGatewayAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipGwAddress[i];
}

// Sets IP DNS address
void setIpDnsAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipDnsAddress[i] = ip[i];
}

// Gets IP gateway address
void getIpDnsAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipDnsAddress[i];
}

// Sets IP time server address
void setIpTimeServerAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipTimeServerAddress[i] = ip[i];
}

// Gets IP time server address
void getIpTimeServerAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipTimeServerAddress[i];
}

// Sets IP time server address
void setIpMqttBrokerAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipTimeServerAddress[i] = ip[i];
}

// Gets IP time server address
void getIpMqttBrokerAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipTimeServerAddress[i];
}

// everything above just updates/sets the network settings

// Calculate sum of words
// Must use getEtherChecksum to complete 1's compliment addition
// this function loops through each byte, grabbing the upper and lower
// in a 16 bit word, then just adds accordingly
void sumIpWords(void* data, uint16_t sizeInBytes, uint32_t* sum)
{
    uint8_t* pData = (uint8_t*)data;
    uint16_t i;
    uint8_t phase = 0;
    uint16_t data_temp;
    for (i = 0; i < sizeInBytes; i++)
    {
        if (phase)
        {
            data_temp = *pData;
            *sum += data_temp << 8; // this is adding the upper byte
        }
        else
          *sum += *pData;   // this is adding the lower byte
        phase = 1 - phase;  // this switches between 0 and 1 so we add low then high, and repeat
        pData++;
    }
}

// Completes 1's compliment addition by folding carries back into field
// this basically handles the overflow past bit 15, and carries it back
// to be added in the lower byte
uint16_t getIpChecksum(uint32_t sum)
{
    uint16_t result;
    // this is based on rfc1071
    while ((sum >> 16) > 0) // if overflow past 16 bits
      sum = (sum & 0xFFFF) + (sum >> 16);   // then add what ever overflowed back in
    result = sum & 0xFFFF;
    return ~result; // this inverts the bits, hence the name ones complement checksum
}

// this is what we call to actually calculate the checksum, the other functions are called here
// what dr losh was saying in class is that we do not include checksum variable itself
// in the calculation, so we sum to before it and after it
// then finish the checksum for any overflow
void calcIpChecksum(ipHeader* ip)
{
    // 32-bit sum over ip header
    uint32_t sum = 0;
    sumIpWords(ip, 10, &sum);                               // sum everything before checksum
    sumIpWords(ip->sourceIp, (ip->size * 4) - 12, &sum);    // sum everything after checksum (to end of header; data not included)
    ip->headerChecksum = getIpChecksum(sum);                // finish the checksum calculation
}
