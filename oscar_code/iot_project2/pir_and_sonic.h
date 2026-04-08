#ifndef PIR_AND_SONIC_H_
#define PIR_AND_SONIC_H_

#include <stdint.h>
#include <stdbool.h>

#define SENSORS_MQTT_TOPIC "tm4c/sensors"

void initPirAndSonicHw(void);
void initPirAndSonic(void);

bool pirMotionDetected(void);
bool isPirEnabled(void);
bool isSonicEnabled(void);
void setPirEnabled(bool enabled);
void setSonicEnabled(bool enabled);
void stopPirAndSonic(void);

void startSensorSampling(void);
void stopSensorSampling(void);
bool isSensorSampleRequested(void);
void clearSensorSampleRequest(void);

bool measureUltrasonicDistanceCm(uint16_t *distanceCm);
void queueSensorMqttPublish(bool pirDetected, bool distanceValid, uint16_t distanceCm,
                            char *topic, uint16_t topicChars, char *payload, uint16_t payloadChars);

#endif
