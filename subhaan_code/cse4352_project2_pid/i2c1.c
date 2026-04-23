// I2C1 Library
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: EK-TM4C123GXL
// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

// Hardware configuration:
// I2C devices on I2C bus 1 with 2kohm pullups on SDA and SCL

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

/*
 *  I2C = Inter-Integrated Circuit
 *
 *  Synchronous, and very similar to SPI but with 2 wires basically for SDA and SCL
 *
 *  SDA: Serial Data which is used by both master/slave to send data
 *  SCL: Serial Clock which is controlled by master to synchronize everything
 *
 *  I2C has open drain bus meaning that they are always idle high and need
 *  external pull up resistors to pull it up to 3.3 V successfully
 *
 *  When the SDA line gets pulled low that means we are transmitting data over the bus
 *  Also, it only has 2 lines because when we send data we will send the address
 *  with it to whatever device needs it. So each data message we send has a
 *  7 bit address + actual data
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include "tm4c123gh6pm.h"
#include "gpio.h"
#include "i2c1.h"

// Pins
#define I2C1SCL PORTA,6
#define I2C1SDA PORTA,7

//-----------------------------------------------------------------------------
// Global variables
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void initI2c1(void)
{
    // Enable clocks
    SYSCTL_RCGCI2C_R |= SYSCTL_RCGCI2C_R1;
    _delay_cycles(3);
    enablePort(PORTA);

    // Configure I2C
    selectPinPushPullOutput(I2C1SCL);                       // set SCL to be output
    setPinAuxFunction(I2C1SCL, GPIO_PCTL_PA6_I2C1SCL);      // set PA6 to I2CSCL

    enablePinPullup(I2C1SCL);   // enable internal pull up resistors since not using external *FOR IOT

    selectPinOpenDrainOutput(I2C1SDA);                      // set SDA to open drain for I2C to work
    setPinAuxFunction(I2C1SDA, GPIO_PCTL_PA7_I2C1SDA);      // set PA7 to I2C1SDA

    enablePinPullup(I2C1SDA);   // same thing as above

    // Configure I2C1 peripheral
    I2C1_MCR_R = 0;                                     // disable master config register before modifying
    // set master timer period register to set the clock to 100 kbps
    I2C1_MTPR_R = 19;                                   // (40MHz/2) / (6+4) / (19+1) = 100kbps
    I2C1_MCR_R = I2C_MCR_MFE;                           // set tm4c to be master
    I2C1_MCS_R = I2C_MCS_STOP;                          // stop the bus to make sure its idle
}

// For simple devices with a single internal register
void writeI2c1Data(uint8_t add, uint8_t data)
{
    // first set the master slave address register to the 7 bit address shifted
    // left 1 to make room for R/S bit, here R/S bit = 0 for write/transmit
    I2C1_MSA_R = add << 1 | 0; // add:r/~w=0
    I2C1_MDR_R = data;          // then load the data we want to send to master data register
    I2C1_MICR_R = I2C_MICR_IC;  // clear master interrupt status
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;    // here we pull SDA low to start, send the address/data, then release SDA to go back high
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);                  // wait until sending data has finished
}

uint8_t readI2c1Data(uint8_t add)
{
    // same thing but now we set R/S bit = 1 for reading
    I2C1_MSA_R = (add << 1) | 1; // add:r/~w=1
    I2C1_MICR_R = I2C_MICR_IC;      // clear interrupt status
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;    // same thing but now we read
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);                  // wait until we finish reading
    return I2C1_MDR_R;
}

// For devices with multiple registers
void writeI2c1Register(uint8_t add, uint8_t reg, uint8_t data)
{
    // send address and register
    I2C1_MSA_R = add << 1; // add:r/~w=0
    I2C1_MDR_R = reg;
    I2C1_MICR_R = I2C_MICR_IC;
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);

    // write data to register
    I2C1_MDR_R = data;
    I2C1_MICR_R = I2C_MICR_IC;
    I2C1_MCS_R = I2C_MCS_RUN | I2C_MCS_STOP;
    while (!(I2C1_MRIS_R & I2C_MRIS_RIS));
}

void writeI2c1Registers(uint8_t add, uint8_t reg, const uint8_t data[], uint8_t size)
{
    uint8_t i;
    // send address and register
    I2C1_MSA_R = add << 1; // add:r/~w=0
    I2C1_MDR_R = reg;
    if (size == 0)
    {
        I2C1_MICR_R = I2C_MICR_IC;
        I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
    }
    else
    {
        I2C1_MICR_R = I2C_MICR_IC;
        I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
        // first size-1 bytes
        for (i = 0; i < size-1; i++)
        {
            I2C1_MDR_R = data[i];
            I2C1_MICR_R = I2C_MICR_IC;
            I2C1_MCS_R = I2C_MCS_RUN;
            while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
        }
        // last byte
        I2C1_MDR_R = data[size-1];
        I2C1_MICR_R = I2C_MICR_IC;
        I2C1_MCS_R = I2C_MCS_RUN | I2C_MCS_STOP;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
    }
}

uint8_t readI2c1Register(uint8_t add, uint8_t reg)
{
    // set internal register counter in device
    I2C1_MSA_R = add << 1 | 0; // add:r/~w=0
    I2C1_MDR_R = reg;
    I2C1_MICR_R = I2C_MICR_IC;
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);

    // read data from register
    I2C1_MSA_R = (add << 1) | 1; // add:r/~w=1
    I2C1_MICR_R = I2C_MICR_IC;
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
    return I2C1_MDR_R;
}

void readI2c1Registers(uint8_t add, uint8_t reg, uint8_t data[], uint8_t size)
{
    uint8_t i = 0;
    // send address and register number
    I2C1_MSA_R = add << 1; // add:r/~w=0
    I2C1_MDR_R = reg;
    I2C1_MICR_R = I2C_MICR_IC;
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);

    if (size == 1)
    {
        // add and read one byte
        I2C1_MSA_R = (add << 1) | 1; // add:r/~w=1
        I2C1_MICR_R = I2C_MICR_IC;
        I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
        data[i++] = I2C1_MDR_R;
    }
    else if (size > 1)
    {
        // add and first byte of read with ack
        I2C1_MSA_R = (add << 1) | 1; // add:r/~w=1
        I2C1_MICR_R = I2C_MICR_IC;
        I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_ACK;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
        data[i++] = I2C1_MDR_R;
        // read size-2 bytes with ack
        while (i < size-1)
        {
            I2C1_MICR_R = I2C_MICR_IC;
            I2C1_MCS_R = I2C_MCS_RUN | I2C_MCS_ACK;
            while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
            data[i++] = I2C1_MDR_R;
        }
        // last byte of read with nack
        I2C1_MICR_R = I2C_MICR_IC;
        I2C1_MCS_R = I2C_MCS_RUN | I2C_MCS_STOP;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
        data[i++] = I2C1_MDR_R;
    }
}

// added function to write easily to nfc pn532 chip
// basically will continue writing for size bytes after the address
void writeI2c1Bytes(uint8_t add, const uint8_t data[], uint8_t size)
{
    uint8_t i;

    if (size == 0)
        return;

    // set R/S bit to 0 for writing and shift address left 1 bit
    I2C1_MSA_R = add << 1 | 0;

    // load the first byte into data register
    I2C1_MDR_R = data[0];
    I2C1_MICR_R = I2C_MICR_IC;  // clear interrupt status

    // if we are only sending 1 byte then send start/stop together
    // like other functions
    if (size == 1)
    {
        I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
        return;
    }

    // if we are sending more than 1 byte we should start streaming
    // then go into loop to send all of them, THEN stop transmitting
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);

    // here we send all of the other bytes
    for (i = 1; i < size - 1; i++)
    {
        I2C1_MDR_R = data[i];
        I2C1_MICR_R = I2C_MICR_IC;
        I2C1_MCS_R = I2C_MCS_RUN;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
    }

    // then we send the last byte and send stop to the line
    I2C1_MDR_R = data[size - 1];
    I2C1_MICR_R = I2C_MICR_IC;
    I2C1_MCS_R = I2C_MCS_RUN | I2C_MCS_STOP;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
}

// added function to also easily read from nfc pn532 chip
void readI2c1Bytes(uint8_t add, uint8_t data[], uint8_t size)
{
    uint8_t i = 0;

    if (size == 0)
        return;

    // set R/S bit to 1 for reading now and shift address left 1 bit
    I2C1_MSA_R = (add << 1) | 1;
    I2C1_MICR_R = I2C_MICR_IC;      // clear interrupts

    // if we are only reading 1 byte do same thing as other functions
    if (size == 1)
    {
        I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
        data[0] = I2C1_MDR_R;
        return;
    }

    // if we are reading more than 1 byte start off by reading first byte
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_ACK;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
    data[i++] = I2C1_MDR_R;

    // now read all of the middle bytes
    while (i < size - 1)
    {
        I2C1_MICR_R = I2C_MICR_IC;
        I2C1_MCS_R = I2C_MCS_RUN | I2C_MCS_ACK;
        while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
        data[i++] = I2C1_MDR_R;
    }

    // then read the last byte and finally send stop instead of ACK
    I2C1_MICR_R = I2C_MICR_IC;
    I2C1_MCS_R = I2C_MCS_RUN | I2C_MCS_STOP;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
    data[i++] = I2C1_MDR_R;
}

bool pollI2c1Address(uint8_t add)
{
    I2C1_MSA_R = (add << 1) | 1; // add:r/~w=1
    I2C1_MICR_R = I2C_MICR_IC;
    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;
    while ((I2C1_MRIS_R & I2C_MRIS_RIS) == 0);
    return !(I2C1_MCS_R & I2C_MCS_ERROR);
}

bool isI2c1Error(void)
{
    return (I2C1_MCS_R & I2C_MCS_ERROR);
}
