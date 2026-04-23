#ifndef PIR_AND_SONIC_H_
#define PIR_AND_SONIC_H_

#include <stdint.h>
#include <stdbool.h>

void initSensors(void);
bool readPir(void);
bool measureUltrasonicDistance(uint16_t *distance_cm);

#endif
