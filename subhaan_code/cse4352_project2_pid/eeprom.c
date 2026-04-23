// EEPROM functions
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target uC:       TM4C123GH6PM
// System Clock:    -

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#include <stdint.h>
#include "tm4c123gh6pm.h"
#include "eeprom.h"

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// eeprom = electronically erasable programmable read only memory
// on datasheet page 534

// this is non volatile memory meaning it retains data even if
// the microcontroller shuts off so like disk memory
// used in project so that we can save all static ip and other info

void initEeprom(void)
{
    // here we turn on the clock for eeprom
    SYSCTL_RCGCEEPROM_R = SYSCTL_RCGCEEPROM_R0;
    _delay_cycles(3);

    // wait until the working bit is cleared
    // so we know set up is complete
    while (EEPROM_EEDONE_R & EEPROM_EEDONE_WORKING);
}

// EEPROM has 2 KB memory (2048 bytes)
// split into 32 blocks, each block with 16 32-bit words
// so 32*16= 512 total variables basically

// function takes in the address and the data we want to
// write to the address
void writeEeprom(uint16_t add, uint32_t data)
{
    EEPROM_EEBLOCK_R = add >> 4;        // find the block the address is in by dividing by 16
    EEPROM_EEOFFSET_R = add & 0xF;      // find the exact word/offset in the block
    EEPROM_EERDWR_R = data;             // write the data to the address

    // wait until the working bit cleared / everything is done
    while (EEPROM_EEDONE_R & EEPROM_EEDONE_WORKING);
}

uint32_t readEeprom(uint16_t add)
{
    // similarly calculate the block/offset from the address
    // then just read/return the data from the address
    EEPROM_EEBLOCK_R = add >> 4;
    EEPROM_EEOFFSET_R = add & 0xF;
    return EEPROM_EERDWR_R;
}
