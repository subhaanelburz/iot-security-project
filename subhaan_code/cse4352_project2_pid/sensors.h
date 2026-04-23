#ifndef SENSORS_H_
#define SENSORS_H_

#include <stdint.h>
#include <stdbool.h>
#include "nfc.h"

#define TOPIC_NFC_ID        "pid_nfc_id"                // we will publish the id to anyone who requests it
#define TOPIC_PIR_MOTION    "pid_pir_motion"            // publish when ir detects motion, details: on/off
#define TOPIC_ULT_DISTANCE  "pid_ultrasonic_distance"   // publish how far object is, details: cm

#define TOPIC_NFC_ADD       "pid_nfc_add"               // we will subscribe to anyone wanting to add, packet payload: name_group
#define TOPIC_NFC_DELETE    "pid_nfc_delete"            // same thing but delete

// different group names for the integration
#define GROUP_PIR       "pir"
#define GROUP_KNOCK     "knock"
#define GROUP_LOCK      "lock"
#define GROUP_GARAGE    "garage"

// we will publish these when an approved tag scans
#define TOPIC_KNOCK_UNLOCK      "knock_unlock"
#define TOPIC_KNOCK_LOCK        "knock_lock"
#define TOPIC_LOCK_SET_STATE    "lock_set_state"    // lock or unlock as payload
#define TOPIC_GARAGE_OPEN       "garage_open"
#define TOPIC_GARAGE_CLOSE      "garage_close"

#define NFC_TABLE_MAX 8     // max approved tags we save
#define NFC_NAME_MAX 16     // max length of name
#define NFC_GROUP_MAX 8     // max length of group

#define ENROLL_TIMEOUT 5    // how long the pid_nfc_add command waits for a tap to save
#define RELOCK_SECONDS 5    // how long we wait before auto locking again

// struct to save approved nfc tags
typedef struct _nfc_entry
{
    bool valid;
    char name[NFC_NAME_MAX];
    char group[NFC_GROUP_MAX];
    uint8_t uid[NFC_MAX_UID_LENGTH];
    uint8_t uid_len;
}
nfc_entry;

// flags for ethernet command processing
extern bool pir_enabled;
extern bool sonic_enabled;
extern bool pir_print;
extern bool sonic_print;

void initAllSensors(void);
void processNfc(void);
void processSensors(void);
void processRelock(void);
void printTags(void);

#endif
