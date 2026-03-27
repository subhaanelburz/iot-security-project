// ETH0 Library
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

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#include <stdint.h>
#include <stdbool.h>
#include "tm4c123gh6pm.h"
#include "wait.h"
#include "gpio.h"
#include "spi0.h"
#include "eth0.h"

// Pins
#define CS PORTA,3  // chip select = PA3
#define WOL PORTB,3 // wake on LAN = PB3
#define INT PORTC,6 // interrupt = PC6

// Ether registers (on the right)
// ENC28J60 control register addresses (page 14 tables)
// macros on left are common writes to the addresses
// bits [4:0] = 5 bit address value
// bits [6:5] = bank # b/c address split into 4 banks [BSEL1:BSEL0]
// so example address would be: ERXFCON = 0x18 (datasheet) -> 0[00][1 1000] = 0[01][1 1000] = 0x38
#define ERDPTL      0x00
#define ERDPTH      0x01
#define EWRPTL      0x02
#define EWRPTH      0x03
#define ETXSTL      0x04
#define ETXSTH      0x05
#define ETXNDL      0x06
#define ETXNDH      0x07
#define ERXSTL      0x08
#define ERXSTH      0x09
#define ERXNDL      0x0A
#define ERXNDH      0x0B
#define ERXRDPTL    0x0C
#define ERXRDPTH    0x0D
#define ERXWRPTL    0x0E
#define ERXWRPTH    0x0F
#define EIE         0x1B
#define EIR         0x1C
#define RXERIF  0x01
#define TXERIF  0x02
#define TXIF    0x08
#define PKTIF   0x40
#define ESTAT       0x1D
#define CLKRDY  0x01
#define TXABORT 0x02
#define ECON2       0x1E
#define PKTDEC  0x40
#define ECON1       0x1F
#define RXEN    0x04
#define TXRTS   0x08
#define ERXFCON     0x38    // example calculation done above
#define EPKTCNT     0x39
#define MACON1      0x40
#define MARXEN  0x01
#define RXPAUS  0x04
#define TXPAUS  0x08
#define MACON2      0x41
#define MARST   0x80
#define MACON3      0x42
#define FULDPX  0x01
#define FRMLNEN 0x02
#define TXCRCEN 0x10
#define PAD60   0x20
#define MACON4      0x43
#define MABBIPG     0x44
#define MAIPGL      0x46
#define MAIPGH      0x47
#define MACLCON1    0x48
#define MACLCON2    0x49
#define MAMXFLL     0x4A
#define MAMXFLH     0x4B
#define MICMD       0x52
#define MIIRD   0x01
#define MIREGADR    0x54
#define MIWRL       0x56
#define MIWRH       0x57
#define MIRDL       0x58
#define MIRDH       0x59
#define MAADR1      0x60
#define MAADR0      0x61
#define MAADR3      0x62
#define MAADR2      0x63
#define MAADR5      0x64
#define MAADR4      0x65
#define MISTAT      0x6A
#define MIBUSY  0x01
#define ECOCON      0x75

// Ether phy registers
// ENC28J60 physical layer addresses/writes
#define PHCON1      0x00
#define PDPXMD 0x0100
#define PHSTAT1     0x01
#define LSTAT  0x0400
#define PHCON2      0x10
#define HDLDIS 0x0100
#define PHLCON      0x14

// ------------------------------------------------------------------------------
//  Globals
// ------------------------------------------------------------------------------

// ex: next packet at address 0x0727
uint8_t nextPacketLsb = 0x00;   // stores LSB of next packet address (0x27)
uint8_t nextPacketMsb = 0x00;   // stores MSB of next packet address (0x07)

// packet count/number basically, every packet increments this
uint8_t sequenceId = 1; // first packet = 1, second = 2, ...

// hardcoded mac address for tm4c
uint8_t hwAddress[HW_ADD_LENGTH] = {2,3,4,5,6,107}; // changed to 107 for my unique mac address

// ------------------------------------------------------------------------------
//  Structures : are in the header file
// ------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// Buffer is configured as follows
// Receive buffer starts at 0x0000 (bottom 6666 bytes of 8K space)
// Transmit buffer at 01A0A (top 1526 bytes of 8K space)

// chip select is active low, so we set it accordingly
void enableEtherCs(void)
{
    setPinValue(CS, 0);
    _delay_cycles(4);                    // allow line to settle
}

// disable chip select by setting it high
void disableEtherCs(void)
{
    setPinValue(CS, 1);
}

// write a value to a register on the ENC28J60 from page
void writeEtherReg(uint8_t reg, uint8_t data)
{
    enableEtherCs();                        // first enable chip select

    // write to register by masking register address with 0x1F (5 bits)
    // then we add the 3 bit op code for write control register being 010 (page 28)
    // so, [op code][5 bit address] = 010 [0 0000] = OR with 0100
    writeSpi0Data(0x40 | (reg & 0x1F));

    readSpi0Data();         // read spi after writing to clear the line
    writeSpi0Data(data);    // now we write the actual data to the register
    readSpi0Data();         // read spi again to get rid of dummy byte

    disableEtherCs();       // disable chip select after writing to register
}

uint8_t readEtherReg(uint8_t reg)
{
    uint8_t data;
    enableEtherCs();
    writeSpi0Data(0x00 | (reg & 0x1F)); // similar but RCR = 000 op code (page 28/26 again)
    readSpi0Data();                     // read dummy byte
    writeSpi0Data(0);                   // now send dummy byte
    data = readSpi0Data();              // now read the actual data in the register
    disableEtherCs();
    return data;
}

// this is different from write because this allows us
// to mask it with an OR basically
void setEtherReg(uint8_t reg, uint8_t mask)
{
    enableEtherCs();
    writeSpi0Data(0x80 | (reg & 0x1F)); // same as write but now BFS op code (page 28/26)
    readSpi0Data();
    writeSpi0Data(mask);
    readSpi0Data();
    disableEtherCs();
}

// same but now we mask with AND and negation so & with ~ negation
void clearEtherReg(uint8_t reg, uint8_t mask)
{
    enableEtherCs();
    writeSpi0Data(0xA0 | (reg & 0x1F)); // op code BFC = 101
    readSpi0Data();
    writeSpi0Data(mask);
    readSpi0Data();
    disableEtherCs();
}

// ECON1 is a key register that is in all 4 banks (page 14)
// and we can control it to change the active bank the ENC is using
void setEtherBank(uint8_t reg)
{
    clearEtherReg(ECON1, 0x03);     // first clear the bank select bits
    setEtherReg(ECON1, reg >> 5);   // then shift by 5 to get the active bank from bits [6:5]
}

// this is used to write to phy registers (page 21/19)
// datasheet tells to write to MIREGADR, then
// write lower 8 bits to MIWRL and upper 8 bits to MIWRH
void writeEtherPhy(uint8_t reg, uint16_t data)
{
    setEtherBank(MIREGADR);         // set the bank accordingly
    writeEtherReg(MIREGADR, reg);
    writeEtherReg(MIWRL, data & 0xFF);
    writeEtherReg(MIWRH, (data >> 8) & 0xFF);
}

// reading is also from same page 21/19:
// first write to the phy address to MIREGADR
// then set the MICMD.MIIRD bit
// wait 10.24 microseconds and poll MISTAT.BUSY until it is cleared
// then clear the MICMD.MIIRD bit
// and lastly read result from MIRDL and MIRDH
uint16_t readEtherPhy(uint8_t reg)
{
    uint16_t data, dataH;
    setEtherBank(MIREGADR);         // set the bank accordingly
    writeEtherReg(MIREGADR, reg);
    writeEtherReg(MICMD, MIIRD);    // set the bit
    waitMicrosecond(11);
    setEtherBank(MISTAT);
    while ((readEtherReg(MISTAT) & MIBUSY) != 0);   // poll until cleared
    setEtherBank(MICMD);
    writeEtherReg(MICMD, 0);        // clear bit
    data = readEtherReg(MIRDL);     // read lower byte result
    dataH = readEtherReg(MIRDH);    // read upper byte result
    data |= (dataH << 8);           // combine into 16 bit result
    return data;
}

// WBM command basically allows host controller to write
// multiple bytes with the EWRPT pointer being auto incremented (page 31/29)
void startEtherMemWrite(void)
{
    enableEtherCs();        // enable chip select
    writeSpi0Data(0x7A);    // this refers to WBM = 0111 1010 = 0x7A on page 28/26
    readSpi0Data();         // read the dummy byte
}

// now we can use this function to write the data
void writeEtherMem(uint8_t data)
{
    writeSpi0Data(data);    // send the data we want to write to buffer memory
    readSpi0Data();         // read the dummy byte again
}

// will have to manually turn off the WBM writing command by calling this
void stopEtherMemWrite(void)
{
    disableEtherCs();       // disable chip select
}

// similar to before but read buffer memory command which
// lets the host controller read multiple bytes (page 30/28)
void startEtherMemRead(void)
{
    enableEtherCs();        // enable chip select
    writeSpi0Data(0x3A);    // RBM = 0011 1010 = 0x3A on page 28/26
    readSpi0Data();
}

// same thing, use this to read each byte
uint8_t readEtherMem(void)
{
    writeSpi0Data(0);           // send a dummy byte when reading
    return readSpi0Data();      // return whatever byte in buffer memory
}

// use this to end the RBM command
void stopEtherMemRead(void)
{
    disableEtherCs();   // disable chip select
}

// Initializes ethernet device
// Uses order suggested in Chapter 6 of datasheet except 6.4 OST which is first here
void initEther(uint16_t mode)
{
    // Initialize SPI0
    initSpi0(USE_SSI0_RX);
    setSpi0BaudRate(10e6, 40e6);
    setSpi0Mode(0, 0);

    // Enable clocks
    enablePort(PORTA);
    enablePort(PORTB);
    enablePort(PORTC);

    // Configure pins for ethernet module
    selectPinPushPullOutput(CS);
    selectPinDigitalInput(WOL);
    selectPinDigitalInput(INT);

    // make sure that oscillator start-up timer has expired
    // page 7/5 says we have to wait at least 300 microseconds
    while ((readEtherReg(ESTAT) & CLKRDY) == 0) {}

    // disable transmission and reception of packets
    clearEtherReg(ECON1, RXEN);
    clearEtherReg(ECON1, TXRTS);

    // initialize receive buffer space; this is from page 35/33
    // the total memory is from 0x0000 to 0x1FFF as seen in figure 3-2
    // total 8KB = 8192 bytes, and datasheet says allocate most here
    // and the rest goes to tx
    setEtherBank(ERXSTL);
    writeEtherReg(ERXSTL, LOBYTE(0x0000));  // prof allocates 0x0000 to 0x1A09
    writeEtherReg(ERXSTH, HIBYTE(0x0000));  // meaning 6666 total bytes for RX buffer
    writeEtherReg(ERXNDL, LOBYTE(0x1A09));  // leaving 8192-6666=1526 bytes for TX
    writeEtherReg(ERXNDH, HIBYTE(0x1A09));
   
    // initialize receiver write and read ptrs
    // at startup, will write from 0 to 1A08 only and will not overwrite rd ptr
    // similarly setting up more receive registers
    writeEtherReg(ERXWRPTL, LOBYTE(0x0000));
    writeEtherReg(ERXWRPTH, HIBYTE(0x0000));
    writeEtherReg(ERXRDPTL, LOBYTE(0x1A09));
    writeEtherReg(ERXRDPTH, HIBYTE(0x1A09));
    writeEtherReg(ERDPTL, LOBYTE(0x0000));
    writeEtherReg(ERDPTH, HIBYTE(0x0000));

    // setup receive filter
    // always check CRC, use OR mode
    // part of section 6.3, basically will discard packets with invalid CRC (page 50/48)
    setEtherBank(ERXFCON);
    writeEtherReg(ERXFCON, (mode | ETHER_CHECKCRC) & 0xFF);

    // bring mac out of reset from section 6.5
    setEtherBank(MACON2);
    writeEtherReg(MACON2, 0);
  
    // enable mac rx, enable pause control for full duplex
    writeEtherReg(MACON1, TXPAUS | RXPAUS | MARXEN);

    // enable padding to 60 bytes (no runt packets)
    // add crc to tx packets, set full or half duplex
    if ((mode & ETHER_FULLDUPLEX) != 0)
        writeEtherReg(MACON3, FULDPX | FRMLNEN | TXCRCEN | PAD60);
    else
        writeEtherReg(MACON3, FRMLNEN | TXCRCEN | PAD60);

    // leave MACON4 as reset

    // set maximum rx packet size
    writeEtherReg(MAMXFLL, LOBYTE(1518));   // 1518 is the max ethernet frame size
    writeEtherReg(MAMXFLH, HIBYTE(1518));

    // set back-to-back inter-packet gap to 9.6us
    // this is from bottom of page 38/36
    if ((mode & ETHER_FULLDUPLEX) != 0)
        writeEtherReg(MABBIPG, 0x15);       // full duplex should be 0x15
    else
        writeEtherReg(MABBIPG, 0x12);       // half duplex should be 0x12

    // set non-back-to-back inter-packet gap registers
    // from section 6.5 steps 6 and 7
    writeEtherReg(MAIPGL, 0x12);
    writeEtherReg(MAIPGH, 0x0C);

    // leave collision window MACLCON2 as reset

    // initialize phy duplex
    // from section 6.6
    if ((mode & ETHER_FULLDUPLEX) != 0)
        writeEtherPhy(PHCON1, PDPXMD);  // full duplex uses PDPXMD
    else
        writeEtherPhy(PHCON1, 0);       // half duplex doesnt set PHCON1

    // disable phy loopback if in half-duplex mode
    writeEtherPhy(PHCON2, HDLDIS);      // it sets PHCON2 to HDLDIS

    // Flash LEDA and LEDB
    writeEtherPhy(PHLCON, 0x0880);  // from page 11/9 1000 = on
    waitMicrosecond(100000);

    // set LEDA (link status) and LEDB (tx/rx activity)
    // stretch LED on to 40ms (default)
    writeEtherPhy(PHLCON, 0x0472);  // display link status (0100), 0111 (tx/rx), 0010 (stretch LED by TLSTRCH?)

    // enable reception
    setEtherReg(ECON1, RXEN);
}

// Returns true if link is up
bool isEtherLinkUp(void)
{
    return (readEtherPhy(PHSTAT1) & LSTAT) != 0; // page 25
}

// Returns TRUE if packet received
bool isEtherDataAvailable(void)
{
    return ((readEtherReg(EIR) & PKTIF) != 0);  // page 68
}

// Returns true if rx buffer overflowed after correcting the problem
bool isEtherOverflow(void)
{
    bool err;
    err = (readEtherReg(EIR) & RXERIF) != 0;    // also page 68, different register
    if (err)
        clearEtherReg(EIR, RXERIF); // clear the error flag bit
    return err;
}

// Returns up to max_size characters in data buffer
// Returns number of bytes copied to buffer
// Contents written are 16-bit size, 16-bit status, payload excl crc
uint16_t getEtherPacket(etherHeader *ether, uint16_t maxSize)
{
    uint16_t i = 0, size, tmp16, status;
    uint8_t *packet = (uint8_t*)ether;

    // enable read from FIFO buffers
    startEtherMemRead();

    // get next packet information
    nextPacketLsb = readEtherMem(); // page 45 shows sample layout
    nextPacketMsb = readEtherMem();

    // actual packet layout was struct in header: size, status, data

    // calc size
    // don't return crc, instead return size + status, so size is correct
    size = readEtherMem();  // read lower byte
    tmp16 = readEtherMem(); // read upper byte
    size |= (tmp16 << 8);   // combine to get actual size

    // get status (currently unused)
    status = readEtherMem();    // read lower byte
    tmp16 = readEtherMem();     // read upper byte
    status |= (tmp16 << 8);     // combine

    // copy data
    if (size > maxSize)
        size = maxSize; // clamp the size if greater than max size

    // nowe we can read the actual data from the packet we received
    while (i < size)
        packet[i++] = readEtherMem();

    // end read from FIFO buffers
    stopEtherMemRead();

    // advance read pointer
    setEtherBank(ERXRDPTL);
    writeEtherReg(ERXRDPTL, nextPacketLsb); // hw ptr
    writeEtherReg(ERXRDPTH, nextPacketMsb);
    writeEtherReg(ERDPTL, nextPacketLsb);   // dma rd ptr
    writeEtherReg(ERDPTH, nextPacketMsb);

    // decrement packet counter so that PKTIF is maintained correctly
    setEtherReg(ECON2, PKTDEC);

    return size;
}

// Writes a packet
bool putEtherPacket(etherHeader *ether, uint16_t size)
{
    uint16_t i;
    uint8_t *packet = (uint8_t*) ether;

    // clear out any tx errors
    if ((readEtherReg(EIR) & TXERIF) != 0)  // if there is a tx error
    {
        clearEtherReg(EIR, TXERIF); // clear previous errors
        setEtherReg(ECON1, TXRTS);  // reset transmit by toggling (page 60/58)
        clearEtherReg(ECON1, TXRTS);
    }

    // set DMA start address
    setEtherBank(EWRPTL);
    writeEtherReg(EWRPTL, LOBYTE(0x1A0A));  // start address is 0x1A0A to 0x1FFF (1526 bytes)
    writeEtherReg(EWRPTH, HIBYTE(0x1A0A));

    // start FIFO buffer write
    startEtherMemWrite();

    // write control byte
    writeEtherMem(0);

    // write data
    for (i = 0; i < size; i++)
        writeEtherMem(packet[i]);

    // stop write
    stopEtherMemWrite();
  
    // request transmit
    writeEtherReg(ETXSTL, LOBYTE(0x1A0A));
    writeEtherReg(ETXSTH, HIBYTE(0x1A0A));
    writeEtherReg(ETXNDL, LOBYTE(0x1A0A+size));
    writeEtherReg(ETXNDH, HIBYTE(0x1A0A+size));
    clearEtherReg(EIR, TXIF);
    setEtherReg(ECON1, TXRTS);

    // wait for completion
    while ((readEtherReg(ECON1) & TXRTS) != 0);

    // determine success
    return ((readEtherReg(ESTAT) & TXABORT) == 0);
}

// Converts from host to network order and vice versa
// host (little endian) to network (big endian) for 16 bit; ntohs is same
uint16_t htons(uint16_t value)
{
    return ((value & 0xFF00) >> 8) + ((value & 0x00FF) << 8);
}

// host (little endian) to network (big endian) for 32 bit; ntohl is same
uint32_t htonl(uint32_t value)
{
    return ((value & 0xFF000000) >> 24) + ((value & 0x00FF0000) >> 8) +
           ((value & 0x0000FF00) << 8) + ((value & 0x000000FF) << 24);
}

// return the ethernet id in network (big endian) order
uint16_t getEtherId(void)
{
    return htons(sequenceId);
}

// increment the counter for each packet we transmit
void incEtherId(void)
{
    sequenceId++;   // ethernet id
}

// Sets MAC address
void setEtherMacAddress(uint8_t mac0, uint8_t mac1, uint8_t mac2, uint8_t mac3, uint8_t mac4, uint8_t mac5)
{
    hwAddress[0] = mac0;    // set the array mac values
    hwAddress[1] = mac1;
    hwAddress[2] = mac2;
    hwAddress[3] = mac3;
    hwAddress[4] = mac4;
    hwAddress[5] = mac5;
    setEtherBank(MAADR0);
    writeEtherReg(MAADR5, mac0);    // then set all of the ENC registers for the correct mac values
    writeEtherReg(MAADR4, mac1);    // program in reverse order [1:6] instead of [6:1] page 36/34
    writeEtherReg(MAADR3, mac2);    // prof counted from R0 i think that is only difference
    writeEtherReg(MAADR2, mac3);
    writeEtherReg(MAADR1, mac4);
    writeEtherReg(MAADR0, mac5);
}

// Gets MAC address; litrerally loops through and copies
void getEtherMacAddress(uint8_t mac[HW_ADD_LENGTH])
{
    uint8_t i;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        mac[i] = hwAddress[i];
}
