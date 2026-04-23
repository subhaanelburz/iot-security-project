// Socket Library
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

#ifndef SOCKET_H_
#define SOCKET_H_

#include <stdint.h>
#include <stdbool.h>
#include "ip.h"

// UDP/TCP socket
// the socket is basically a neat struct to store the info of
// who we are talking to; basically info of both sender/receiver
// so that we can simplify function arguments
typedef struct _socket
{
    uint8_t remoteIpAddress[4]; // the ip address of who we are talking to
    uint8_t remoteHwAddress[6]; // the mac address of who we are talking to
    uint16_t remotePort;        // UDP port number of who we are talking to
    uint16_t localPort;         // our UDP port number
    uint32_t sequenceNumber;
    uint32_t acknowledgementNumber;
    uint8_t  state;             // state to check if socket is free to use
} socket;

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void initSockets();
socket * newSocket();
void deleteSocket(socket *s);
void getSocketInfoFromArpResponse(etherHeader *ether, socket *s);
void getSocketInfoFromUdpPacket(etherHeader *ether, socket *s);
void getSocketInfoFromTcpPacket(etherHeader *ether, socket *s);

#endif

