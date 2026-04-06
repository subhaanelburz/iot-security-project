#include <stdint.h>
#include <stdbool.h>
#include "tm4c123gh6pm.h"
#include "gpio.h"
#include "wait.h"

void initSensors()
{
    enablePort(PORTD); //

    // PIR Sensor on PD0
    selectPinDigitalInput(PORTD, 0); //

    // Ultrasonic Trigger on PD1
    selectPinPushPullOutput(PORTD, 1); //

    // Ultrasonic Echo on PD2 (mapped to T1CCP0)
    selectPinDigitalInput(PORTD, 2); //
    setPinAuxFunction(PORTD, 2, 0x7); //

    // Configure Timer 1 for Edge Time Capture
    SYSCTL_RCGCTIMER_R |= SYSCTL_RCGCTIMER_R1;
    _delay_cycles(3);
    TIMER1_CTL_R &= ~TIMER_CTL_TAEN;
    TIMER1_CFG_R = TIMER_CFG_16_BIT;
    TIMER1_TAMR_R = TIMER_TAMR_TACMR | TIMER_TAMR_TAMR_CAP | TIMER_TAMR_TACDIR; // Count up, Capture mode
    TIMER1_CTL_R |= TIMER_CTL_TAEVENT_BOTH; // Capture both edges
    TIMER1_CTL_R |= TIMER_CTL_TAEN;
}

float getDistance()
{
    uint32_t t1, t2;

    // Send 10us Trigger pulse
    setPinValue(PORTD, 1, 1);
    waitMicrosecond(10);
    setPinValue(PORTD, 1, 0);

    // Wait for rising edge and capture t1
    TIMER1_ICR_R = TIMER_ICR_TATOCINT;
    while (!(TIMER1_RIS_R & TIMER_RIS_TATORIS));
    t1 = TIMER1_TAR_R;

    // Wait for falling edge and capture t2
    TIMER1_ICR_R = TIMER_ICR_TATOCINT;
    while (!(TIMER1_RIS_R & TIMER_RIS_TATORIS));
    t2 = TIMER1_TAR_R;

    // Distance = (Time * Speed of Sound) / 2
    // At 40MHz, 1 tick = 0.025us. Speed of sound is ~0.0343 cm/us
    return (float)(t2 - t1) * 0.00042875;
}
