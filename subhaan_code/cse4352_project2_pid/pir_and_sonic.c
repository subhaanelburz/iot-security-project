#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "gpio.h"
#include "wait.h"
#include "pir_and_sonic.h"

// pins used for sensors
#define PIR_INPUT           PORTE,1
#define ULTRASONIC_TRIGGER  PORTE,2 // input trigger on ultrasonic sensor
#define ULTRASONIC_ECHO     PORTE,3 // echo output

// wait 1 ms for echo to go back to being low
#define ECHO_READY_TIMEOUT_US   1000

// echo width will timeout in 38 ms so set ours to 30 ms
#define ECHO_PULSE_TIMEOUT_US   30000

// function to check pin until it matches the value
// returns false if value never reaches on timeout
bool waitForPinValue(PORT port, uint8_t pin, bool value, uint32_t timeoutUs)
{
    while (timeoutUs > 0)
    {
        if (getPinValue(port, pin) == value)
            return true;
        waitMicrosecond(1);
        timeoutUs--;
    }

    return false;
}

void initSensors(void)
{
    enablePort(PORTE);
    _delay_cycles(3);

    // set pir as digital input with pull down resistor to keep it low
    selectPinDigitalInput(PIR_INPUT);
    enablePinPulldown(PIR_INPUT);

    // trigger is output of tm4c, input to ultrasonic
    // which we pulse high for >10us to start transmit burst
    selectPinPushPullOutput(ULTRASONIC_TRIGGER);
    setPinValue(ULTRASONIC_TRIGGER, 0);

    // set the echo to digital input to read the echo width
    // which will allow us to calculate the distance
    selectPinDigitalInput(ULTRASONIC_ECHO);
    enablePinPulldown(ULTRASONIC_ECHO);
}

// returns true if the pir detects motion
bool readPir(void)
{
    return getPinValue(PIR_INPUT);
}

// function to capture the actual distance by triggering
// then converting echo output to distance in cm
bool measureUltrasonicDistance(uint16_t *distance_cm)
{
    uint32_t pulseWidthUs = 0;

    if (distance_cm == NULL)
        return false;

    // make sure echo goes back down to low before triggering again
    if (!waitForPinValue(ULTRASONIC_ECHO, 0, ECHO_READY_TIMEOUT_US))
        return false;

    // send the 10us pulse on trigger
    setPinValue(ULTRASONIC_TRIGGER, 0);
    waitMicrosecond(2);
    setPinValue(ULTRASONIC_TRIGGER, 1);
    waitMicrosecond(10);
    setPinValue(ULTRASONIC_TRIGGER, 0);

    // wait for echo to go high so we can start measuring the pulse width
    if (!waitForPinValue(ULTRASONIC_ECHO, 1, ECHO_PULSE_TIMEOUT_US))
        return false;

    // actually measure the pulse width
    while (getPinValue(ULTRASONIC_ECHO))
    {
        // return false if the width is longer than the timeout
        if (pulseWidthUs >= ECHO_PULSE_TIMEOUT_US)
            return false;
        waitMicrosecond(1);
        pulseWidthUs++;
    }

    // datasheet equation for Dcm = Ew/58
    *distance_cm = pulseWidthUs / 58;
    return true;
}
