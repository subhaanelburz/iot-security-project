// SPI1 Library
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: EK-TM4C123GXL
// Target uC:       TM4C123GH6PM
// System Clock:    -

// Hardware configuration:
// SPI1 Interface:
//   MOSI on PD3 (SSI1Tx)
//   MISO on PD2 (SSI1Rx)
//   ~CS on PD1  (SSI1Fss)
//   SCLK on PD0 (SSI1Clk)

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

/*
 *  SPI = Serial Peripheral Interface
 *
 *  Synchronous, full-duplex communication between the master and the slave.
 *
 *  Basically, it uses a clock and every clock cycle it will send a bit.
 *
 *  There are 2 data lines, MOSI and MISO, which is:
 *
 *  MOSI: Master Out, Slave in (master send data, slave receive)
 *  MISO: Master In, Slave Out (slave send data, master receive)
 *
 *  CS/FSS: Active low signal controlled by the Master, basically:
 *
 *  Low 0 V: Slave is active and listening for data
 *  High 3.3 V: Slave ignores clock and data lines
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include "tm4c123gh6pm.h"
#include "spi1.h"
#include "gpio.h"

// Pins
#define SSI1TX PORTD,3  // PD3 is Master Out, Slave in (Transmit Pin)
#define SSI1RX PORTD,2  // PD2 is Master In, Slave Out (Receive Pin)
#define SSI1FSS PORTD,1 // PD1 is Chip Select / Frame Slave Select (Active Low)
#define SSI1CLK PORTD,0 // PD0 is the SPI (Serial) Clock

//-----------------------------------------------------------------------------
// Global variables
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// Initialize SPI1
void initSpi1(uint32_t pinMask)
{
    // Enable clocks
    SYSCTL_RCGCSSI_R |= SYSCTL_RCGCSSI_R1;
    _delay_cycles(3);
    enablePort(PORTD);

    // Configure SSI1 pins for SPI configuration
    selectPinPushPullOutput(SSI1TX);                    // make transmit pin a digital output
    setPinAuxFunction(SSI1TX, GPIO_PCTL_PD3_SSI1TX);    // set PD3 to SPI transmit
    selectPinPushPullOutput(SSI1CLK);                   // make clock pin a digital output
    setPinAuxFunction(SSI1CLK, GPIO_PCTL_PD0_SSI1CLK);  // set PD0 to SPI Clock
    selectPinPushPullOutput(SSI1FSS);                   // make CS pin a digital output
    if (pinMask & USE_SSI_FSS)
    {
        // if the pin passed into function was the FSS pin, we enable it
        // if this wasnt passed in then the HW doesn't automatically
        // configure the FSS pin
        setPinAuxFunction(SSI1FSS, GPIO_PCTL_PD1_SSI1FSS);
    }
    if (pinMask & USE_SSI_RX)
    {
        // if the pin passed into the function was the RX pin, we enable it
        // if this wasnt passed in then the TM4C does not receive data
        // meaning we only are transmitting/writing
        selectPinDigitalInput(SSI1RX);
        setPinAuxFunction(SSI1RX, GPIO_PCTL_PD2_SSI1RX);
    }

    // Configure the SSI1 as a SPI master, mode 3, 16 bit operation
    SSI1_CR1_R &= ~SSI_CR1_SSE;                        // turn off SSI1 to allow re-configuration
    SSI1_CR1_R = 0;                                    // select master mode for the TM4C
    SSI1_CC_R = 0;                                     // select system clock as the clock source
    SSI1_CR0_R = SSI_CR0_FRF_MOTO | SSI_CR0_DSS_16;    // set SR=0, 16-bit meaning we are only sending 16 bit data frames
}

// Set baud rate as function of instruction cycle frequency
// this function basically divides the SPI clock if you want to
// Basically, spi clock = sysclock / divisor here
void setSpi1BaudRate(uint32_t baudRate, uint32_t fcyc)
{
    uint32_t divisorTimes2 = (fcyc * 2) / baudRate;    // calculate divisor (r) times 2
    SSI1_CR1_R &= ~SSI_CR1_SSE;                        // turn off SSI1 to allow re-configuration
    SSI1_CPSR_R = (divisorTimes2 + 1) >> 1;            // round divisor to nearest integer
    SSI1_CR1_R |= SSI_CR1_SSE;                         // turn on SSI1
}

// Set mode
void setSpi1Mode(uint8_t polarity, uint8_t phase)
{
    SSI1_CR1_R &= ~SSI_CR1_SSE;                        // turn off SSI1 to allow re-configuration
    SSI1_CR0_R &= ~(SSI_CR0_SPH | SSI_CR0_SPO);        // set SPO and SPH as appropriate, this just clears all old settings
    if (polarity)
    {
        // polarity being 1 means the clock will be idle HIGH, meaning active low
        SSI1_CR0_R |= SSI_CR0_SPO;
        enablePinPullup(SSI1CLK); // pull up here will pull up the pin to HIGH when idle
    }
    else
        disablePinPullup(SSI1CLK);  // keep the clock active high
    if (phase)
        SSI1_CR0_R |= SSI_CR0_SPH;  // if phase is 1, that means data is captured on the second clock edge transition
    SSI1_CR1_R |= SSI_CR1_SSE;                         // turn on SSI1
}

// Blocking function that writes data and waits until the tx buffer is empty
void writeSpi1Data(uint32_t data)
{
    SSI1_DR_R = data;               // write to data register, which puts it into TX FIFO
    while (SSI1_SR_R & SSI_SR_BSY); // if the TX FIFO is busy, we wait *so it waits until its empty
}

// Reads data from the rx buffer after a write
uint32_t readSpi1Data()
{
    return SSI1_DR_R;   // gets the data from the RX FIFO
}
