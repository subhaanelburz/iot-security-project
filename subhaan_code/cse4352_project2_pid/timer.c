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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "tm4c123gh6pm.h"
#include "timer.h"

//-----------------------------------------------------------------------------
// Global variables
//-----------------------------------------------------------------------------

#define NUM_TIMERS 10   // max number of timers we can set up at the same time

// each index corresponds to the timer num, so index 0 = timer 0

_callback fn[NUM_TIMERS];       // this holds the function pointers to call for each timer
uint32_t period[NUM_TIMERS];    // the number of seconds the timer was set for
uint32_t ticks[NUM_TIMERS];     // the current countdown value of the timer in seconds
bool reload[NUM_TIMERS];        // true for periodic timer and false for oneshot timer

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void initTimer()
{
    uint8_t i;

    // turn on the clock for timer4
    SYSCTL_RCGCTIMER_R |= SYSCTL_RCGCTIMER_R4;
    _delay_cycles(3);

    // Configure Timer 4 for 1 sec tick
    TIMER4_CTL_R &= ~TIMER_CTL_TAEN;                 // turn-off timer before reconfiguring
    TIMER4_CFG_R = TIMER_CFG_32_BIT_TIMER;           // configure as 32-bit timer (A+B)
    TIMER4_TAMR_R = TIMER_TAMR_TAMR_PERIOD;          // configure for periodic mode (count down)

    // 40 MHz clock so, 40/40 = 1s reload time
    TIMER4_TAILR_R = 40000000;                       // set load value (1 Hz rate)
    TIMER4_CTL_R |= TIMER_CTL_TAEN;                  // turn-on timer
    TIMER4_IMR_R |= TIMER_IMR_TATOIM;                // turn-on interrupt
    NVIC_EN2_R |= 1 << (INT_TIMER4A-80);             // turn-on interrupt 86 (TIMER4A)

    // initialize all of the arrays for the timers
    for (i = 0; i < NUM_TIMERS; i++)
    {
        period[i] = 0;
        ticks[i] = 0;
        fn[i] = NULL;
        reload[i] = false;
    }
}

bool startOneshotTimer(_callback callback, uint32_t seconds)
{
    uint8_t i = 0;
    bool found = false;

    // we loop through the timers to find the first open slot
    while (i < NUM_TIMERS && !found)
    {
        found = fn[i] == NULL;      // if no callback timer that means we have an open slot

        if (found)
        {
            period[i] = seconds;    // set timer duration
            ticks[i] = seconds;     // set timer start value
            fn[i] = callback;       // set timer callback function
            reload[i] = false;      // set reload false b/c oneshot timer
        }

        i++;
    }

    return found;   // return true if timer successfully enabled
}

bool startPeriodicTimer(_callback callback, uint32_t seconds)
{
    uint8_t i = 0;
    bool found = false;

    // we loop through the timers to find the first open slot
    while (i < NUM_TIMERS && !found)
    {
        found = fn[i] == NULL;  // if no callback timer that means we have an open slot

        if (found)
        {
            period[i] = seconds;    // set timer duration
            ticks[i] = seconds;     // set timer start value
            fn[i] = callback;       // set timer callback function
            reload[i] = true;       // set reload true b/c periodic timer
        }

        i++;
    }

    return found;   // return true if timer successfully enabled
}

bool stopTimer(_callback callback)
{
     uint8_t i = 0;
     bool found = false;

     // we loop through to find the timer we want to stop
     while (i < NUM_TIMERS && !found)
     {
         found = fn[i] == callback; // find the timer using callback function

         if (found)
         {
             ticks[i] = 0;          // set the countdown value to 0
             fn[i] = NULL;          // remove the callback function
         }

         i++;
     }

     return found;  // return true if timer successfully disabled
}

bool restartTimer(_callback callback)
{
     uint8_t i = 0;
     bool found = false;

     // loop through to find timer we want to restart
     while (i < NUM_TIMERS && !found)
     {
         found = fn[i] == callback; // identify using callback function

         if (found)
         {
             ticks[i] = period[i];  // restart countdown value to the original timer duration
         }

         i++;
     }

     return found;  // return true if timer successfully restarted
}

// main timer ISR function called every second
void tickIsr()
{
    uint8_t i;

    // we loop through all of the timers to update them
    for (i = 0; i < NUM_TIMERS; i++)
    {
        if (ticks[i] != 0)  // if the timer is still running
        {
            ticks[i]--;     // subract a second from countdown value

            // if the timer just hit zero
            if (ticks[i] == 0)
            {
                if (reload[i])  // reload the timer if periodic
                {
                    ticks[i] = period[i];
                }

                // then call the callback function for the timers handler
                (*fn[i])();

                if (!reload[i]) // if it was a one shot timer
                {
                    fn[i] = NULL;   // we remove the callback function to clear the slot
                }
            }
        }
    }

    TIMER4_ICR_R = TIMER_ICR_TATOCINT;  // clear interrupt status so we can enter ISR again
}

// Placeholder random number function
// this returns the current coundown value of timer 4
// so it does act like a random number gen from 0 to 40,000,000
uint32_t random32()
{
    return TIMER4_TAV_R;
}
