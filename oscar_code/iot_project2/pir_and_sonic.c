#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "gpio.h"
#include "wait.h"
#include "timer.h"
#include "pir_and_sonic.h"

// Sensor pins
#define PIR_SENSOR_INPUT   PORTE,1
#define ULTRASONIC_TRIGGER PORTE,2
// HC-SR04 echo is a 5V signal on most modules. Use a level shifter or divider before PE3.
#define ULTRASONIC_ECHO    PORTE,3

#define ULTRASONIC_READY_TIMEOUT_US 1000
#define ULTRASONIC_ECHO_TIMEOUT_US  30000

static volatile bool sensorSampleRequested = false;
static bool pirEnabled = true;
static bool sonicEnabled = true;

static void callbackSensorSampleTimer(void)
{
    sensorSampleRequested = true;
}

static bool waitForPinValue(PORT port, uint8_t pin, bool value, uint32_t timeoutUs)
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

void initPirAndSonicHw(void)
{
    enablePort(PORTE);

    selectPinDigitalInput(PIR_SENSOR_INPUT);
    enablePinPulldown(PIR_SENSOR_INPUT);

    selectPinPushPullOutput(ULTRASONIC_TRIGGER);
    setPinValue(ULTRASONIC_TRIGGER, 0);

    selectPinDigitalInput(ULTRASONIC_ECHO);
    enablePinPulldown(ULTRASONIC_ECHO);
}

void initPirAndSonic(void)
{
    pirEnabled = true;
    sonicEnabled = true;
    stopSensorSampling();
}

bool pirMotionDetected(void)
{
    return getPinValue(PIR_SENSOR_INPUT);
}

bool isPirEnabled(void)
{
    return pirEnabled;
}

bool isSonicEnabled(void)
{
    return sonicEnabled;
}

void setPirEnabled(bool enabled)
{
    pirEnabled = enabled;
    if (!enabled)
        stopSensorSampling();
}

void setSonicEnabled(bool enabled)
{
    sonicEnabled = enabled;
    if (!enabled)
        stopSensorSampling();
}

void stopPirAndSonic(void)
{
    pirEnabled = false;
    sonicEnabled = false;
    stopSensorSampling();
}

void startSensorSampling(void)
{
    sensorSampleRequested = true;
    if (!restartTimer(callbackSensorSampleTimer))
        startPeriodicTimer(callbackSensorSampleTimer, 1);
}

void stopSensorSampling(void)
{
    stopTimer(callbackSensorSampleTimer);
    sensorSampleRequested = false;
}

bool isSensorSampleRequested(void)
{
    return sensorSampleRequested;
}

void clearSensorSampleRequest(void)
{
    sensorSampleRequested = false;
}

bool measureUltrasonicDistanceCm(uint16_t *distanceCm)
{
    uint32_t pulseWidthUs = 0;

    if (distanceCm == NULL)
        return false;

    if (!waitForPinValue(ULTRASONIC_ECHO, 0, ULTRASONIC_READY_TIMEOUT_US))
        return false;

    setPinValue(ULTRASONIC_TRIGGER, 0);
    waitMicrosecond(2);
    setPinValue(ULTRASONIC_TRIGGER, 1);
    waitMicrosecond(10);
    setPinValue(ULTRASONIC_TRIGGER, 0);

    if (!waitForPinValue(ULTRASONIC_ECHO, 1, ULTRASONIC_ECHO_TIMEOUT_US))
        return false;

    while (getPinValue(ULTRASONIC_ECHO))
    {
        if (pulseWidthUs >= ULTRASONIC_ECHO_TIMEOUT_US)
            return false;
        waitMicrosecond(1);
        pulseWidthUs++;
    }

    *distanceCm = pulseWidthUs / 58;
    return true;
}

void queueSensorMqttPublish(bool pirDetected, bool distanceValid, uint16_t distanceCm,
                            char *topic, uint16_t topicChars, char *payload, uint16_t payloadChars)
{
    snprintf(topic, topicChars, "%s", SENSORS_MQTT_TOPIC);

    if (pirDetected && distanceValid)
        snprintf(payload, payloadChars, "{\"pir\":1,\"distance_cm\":%u}", distanceCm);
    else if (pirDetected)
        snprintf(payload, payloadChars, "{\"pir\":1,\"distance_cm\":null}");
    else
        snprintf(payload, payloadChars, "{\"pir\":0,\"distance_cm\":null}");
}
