// Timer Service Library
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: EK-TM4C123GXL
// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

// Hardware configuration:
// Timer 4

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#ifndef TIMER_H_
#define TIMER_H_

// create a type for function pointers basically
// it holds the address of a function that returns void w/ no arguments
// so the callback functions can be used an interrupt handler for any timers we init
typedef void (*_callback)();

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void initTimer();
bool startOneshotTimer(_callback callback, uint32_t seconds);
bool startPeriodicTimer(_callback callback, uint32_t seconds);
bool stopTimer(_callback callback);
bool restartTimer(_callback callback);
void tickIsr();
uint32_t random32();

#endif
