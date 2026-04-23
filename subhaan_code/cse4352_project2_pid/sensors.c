#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "sensors.h"
#include "nfc.h"
#include "pir_and_sonic.h"
#include "timer.h"
#include "mqtt.h"
#include "uart0.h"

// create our global table of approved tags
nfc_entry nfc_table[NFC_TABLE_MAX];

bool enroll_pending = false;        // flag when we are adding a tag
char enroll_name[NFC_NAME_MAX];     // the name of the tag we are adding
char enroll_group[NFC_GROUP_MAX];   // name of group we are adding

// flags and array to remember which group to relock after timeout
bool relock_pending = false;
bool relock_expired = false;
char relock_group[NFC_GROUP_MAX];

bool nfc_poll_due = false;      // flag to check the nfc
bool enroll_expired = false;    // flag when enrollment period ends

// the last uid we scanned so we can prevent spam
uint8_t last_uid[NFC_MAX_UID_LENGTH];
uint8_t last_uid_len = 0;

bool sensor_poll_due = false;   // flag to poll the sensors
bool last_pir_state = false;    // flag to only publish pir state when it changes
bool pir_enabled = true;        // flag to show pir sensor enabled
bool sonic_enabled = true;      // flag to show ultrasonic sensor enabled
bool pir_print = true;          // flag to print pir data
bool sonic_print = true;        // flag to print ultrasonic data

// called every second to try reading tag
void callbackNfcPoll(void)
{
    nfc_poll_due = true;
}

// after enrollment period ends then set flag to expired
void callbackEnrollTimeout(void)
{
    enroll_expired = true;
}

// called every second to check the sensors
void callbackSensorPoll(void)
{
    sensor_poll_due = true;
}

// called 5s after unlock was sent to the door can auto relock
void callbackRelockTimer(void)
{
    relock_expired = true;
}

// function to print the uid in uart
void printUidHex(const uint8_t uid[], uint8_t len)
{
    char buf[6];
    uint8_t i;

    for (i = 0; i < len; i++)
    {
        snprintf(buf, sizeof(buf), "%02X", uid[i]);
        putsUart0(buf);
        if (i < len - 1)
            putcUart0(':');
    }
}

// look through nfc tag table to find one that matches
// checks if valid, length matches, and uid matches; returns location
nfc_entry* findEntryByUid(const uint8_t uid[], uint8_t len)
{
    uint8_t i;

    for (i = 0; i < NFC_TABLE_MAX; i++)
    {
        if (!nfc_table[i].valid)
            continue;
        if (nfc_table[i].uid_len != len)
            continue;
        if (memcmp(nfc_table[i].uid, uid, len) == 0)
            return &nfc_table[i];
    }

    return NULL;
}

// save or overwrite an entry into the nfc table
// overwrite meaning if same name/group is added then the uid is updated
void saveNfcEntry(const char *name, const char *group, const uint8_t uid[], uint8_t len)
{
    uint8_t i;
    int free_slot = -1;

    for (i = 0; i < NFC_TABLE_MAX; i++)
    {
        // if we find a valid entry with the same name and group overwrite it
        if (nfc_table[i].valid
                && strncmp(nfc_table[i].name, name, NFC_NAME_MAX) == 0
                && strncmp(nfc_table[i].group, group, NFC_GROUP_MAX) == 0)
        {
            memcpy(nfc_table[i].uid, uid, len);
            nfc_table[i].uid_len = len;
            return;
        }

        // otherwise look for a free slot and save the index
        if (!nfc_table[i].valid && free_slot < 0)
            free_slot = i;
    }

    // if the index never gets updated the table is full so return
    if (free_slot < 0)
        return;

    // save the uid into the table by updating all fields
    nfc_table[free_slot].valid = true;
    strncpy(nfc_table[free_slot].name, name, NFC_NAME_MAX - 1);
    nfc_table[free_slot].name[NFC_NAME_MAX - 1] = 0;
    strncpy(nfc_table[free_slot].group, group, NFC_GROUP_MAX - 1);
    nfc_table[free_slot].group[NFC_GROUP_MAX - 1] = 0;
    memcpy(nfc_table[free_slot].uid, uid, len);
    nfc_table[free_slot].uid_len = len;
}

// delete an nfc tag from the table using its name and group
bool deleteNfcEntry(const char *name, const char *group)
{
    uint8_t i;

    for (i = 0; i < NFC_TABLE_MAX; i++)
    {
        if (!nfc_table[i].valid)
            continue;
        if (strncmp(nfc_table[i].name, name, NFC_NAME_MAX) == 0
                && strncmp(nfc_table[i].group, group, NFC_GROUP_MAX) == 0)
        {
            nfc_table[i].valid = false;
            return true;
        }
    }

    return false;
}

// called when we get nfc add message and we start the 5s enrollment window
void armEnrollment(const char *name, const char *group)
{
    strncpy(enroll_name, name, NFC_NAME_MAX - 1);
    enroll_name[NFC_NAME_MAX - 1] = 0;
    strncpy(enroll_group, group, NFC_GROUP_MAX - 1);
    enroll_group[NFC_GROUP_MAX - 1] = 0;
    enroll_pending = true;
    enroll_expired = false;

    putsUart0("\r\nadding tag: ");
    putsUart0(enroll_name);
    putsUart0("_");
    putsUart0(enroll_group);
    putsUart0(", tap within 5 seconds\r\n");

    // restart the timer if it was already running
    if (!restartTimer(callbackEnrollTimeout))
        startOneshotTimer(callbackEnrollTimeout, ENROLL_TIMEOUT);
}

// publish the correct unlock/lock message depending on the group
void publishGroupAction(const char *group, bool unlock)
{
    if (strcmp(group, GROUP_KNOCK) == 0)
    {
        publishMqtt(unlock ? TOPIC_KNOCK_UNLOCK : TOPIC_KNOCK_LOCK, "");
    }
    else if (strcmp(group, GROUP_LOCK) == 0)
    {
        publishMqtt(TOPIC_LOCK_SET_STATE, unlock ? "unlock" : "lock");
    }
    else if (strcmp(group, GROUP_GARAGE) == 0)
    {
        publishMqtt(unlock ? TOPIC_GARAGE_OPEN : TOPIC_GARAGE_CLOSE, "");
    }
}

// function to parse the name_group into 2 separate strings
bool parseNameGroup(const char *msg, char *name_out, char *group_out)
{
    const char *underscore;
    uint8_t name_len;

    // find last underscore in the msg
    underscore = strrchr(msg, '_');
    if (underscore == NULL)
    {
        putsUart0("\r\nformat: name_group (pir|knock|lock|garage)\r\n");
        return false;
    }

    // ensure the name is within the length bounds
    name_len = (uint8_t)(underscore - msg);
    if (name_len == 0 || name_len >= NFC_NAME_MAX)
    {
        putsUart0("\r\nname length invalid\r\n");
        return false;
    }

    // otherwise copy the name and group since they are good + add null terminators
    strncpy(name_out, msg, name_len);
    name_out[name_len] = 0;
    strncpy(group_out, underscore + 1, NFC_GROUP_MAX - 1);
    group_out[NFC_GROUP_MAX - 1] = 0;

    // if the nfc tag added doesn't have valid group print error to terminal
    if (strcmp(group_out, GROUP_PIR) != 0 && strcmp(group_out, GROUP_KNOCK) != 0
            && strcmp(group_out, GROUP_LOCK) != 0
            && strcmp(group_out, GROUP_GARAGE) != 0)
    {
        putsUart0("\r\ngroup must be pir|knock|lock|garage\r\n");
        return false;
    }

    return true;
}

// called whenever we received an add/delete for an nfc tag
void handleMqttPublish(const char *topic, const char *message)
{
    char name[NFC_NAME_MAX];
    char group[NFC_GROUP_MAX];

    if (strcmp(topic, TOPIC_NFC_ADD) == 0)
    {
        // if we are adding make sure it is in correct name_group foramt
        if (!parseNameGroup(message, name, group))
            return;
        armEnrollment(name, group); // start enrollment period
    }
    else if (strcmp(topic, TOPIC_NFC_DELETE) == 0)
    {
        if (!parseNameGroup(message, name, group))
            return;

        if (deleteNfcEntry(name, group))
        {
            putsUart0("\r\ndeleted tag: ");
            putsUart0(name);
            putsUart0("_");
            putsUart0(group);
            putsUart0("\r\n");
        }
        else
        {
            putsUart0("\r\nno tag found: ");
            putsUart0(name);
            putsUart0("_");
            putsUart0(group);
            putsUart0("\r\n");
        }
    }
}

void initAllSensors(void)
{
    // start and print if initialized nfc pn532
    if (initNfc())
    {
        putsUart0("NFC ready\n");
    }
    else
    {
        putsUart0("NFC init failed\n");
    }

    // start pir and ultrasonic sensors
    initSensors();

    // start the nfc polling timers to keep checking sensors every second
    startPeriodicTimer(callbackNfcPoll, 1);
    startPeriodicTimer(callbackSensorPoll, 1);
}

// actually process all nfc scans
void processNfc(void)
{
    uint8_t uid[NFC_MAX_UID_LENGTH];
    uint8_t uid_len = 0;
    nfc_entry *entry;
    char name_group[NFC_NAME_MAX + NFC_GROUP_MAX + 1];

    // once the 5s enrollment period is over then set flags to false
    if (enroll_expired)
    {
        enroll_pending = false;
        enroll_expired = false;
    }

    // if we do not need to check nfc then just return
    if (!nfc_poll_due)
        return;
    nfc_poll_due = false;

    // try reading a tag and return if we do not see a tag
    if (!nfcReadUid(uid, &uid_len))
    {
        last_uid_len = 0;
        return;
    }

    // if we already processed the same tag on previous poll
    // then skip it so it doesnt spam unlock the doors
    if (uid_len == last_uid_len && memcmp(uid, last_uid, uid_len) == 0)
        return;

    // save the last uid that we just saw so next poll can skip if its same
    memcpy(last_uid, uid, uid_len);
    last_uid_len = uid_len;

    // if we read a tag and are in enrollment mode save it and return
    if (enroll_pending)
    {
        saveNfcEntry(enroll_name, enroll_group, uid, uid_len);
        putsUart0("\r\nadded tag: ");
        putsUart0(enroll_name);
        putsUart0("_");
        putsUart0(enroll_group);
        putsUart0("\r\n");
        enroll_pending = false;
        stopTimer(callbackEnrollTimeout);
        return;
    }

    // otherwise if we are just reading any tag save the entry if its in the list
    entry = findEntryByUid(uid, uid_len);
    if (entry != NULL)
    {
        // create the full name_group string for the publish message
        snprintf(name_group, sizeof(name_group), "%s_%s", entry->name, entry->group);

        // approved tag so publish name and unlock door
        putsUart0("\r\nscanned: ");
        putsUart0(name_group);
        putsUart0(" (");
        printUidHex(uid, uid_len);
        putsUart0(")\r\n");

        if (isMqttConnected())
        {
            // publish identity to mqtt, send unlock publish msgs, start autorelock timer
            publishMqtt(TOPIC_NFC_ID, name_group);
            publishGroupAction(entry->group, true);
            strncpy(relock_group, entry->group, NFC_GROUP_MAX - 1);
            relock_group[NFC_GROUP_MAX - 1] = 0;
            relock_pending = true;
            relock_expired = false;
            if (!restartTimer(callbackRelockTimer))
                startOneshotTimer(callbackRelockTimer, RELOCK_SECONDS);
        }
    }
    else
    {
        // tag that we do not recognize so publish and print unknown
        putsUart0("\r\nscanned: unknown (");
        printUidHex(uid, uid_len);
        putsUart0(")\r\n");

        if (isMqttConnected())
            publishMqtt(TOPIC_NFC_ID, "unknown");
    }
}

// print all of the valid entries in the nfc tag approved table
void printTags(void)
{
    uint8_t i;
    uint8_t count = 0;

    putsUart0("approved tags:\r\n");

    for (i = 0; i < NFC_TABLE_MAX; i++)
    {
        if (!nfc_table[i].valid)
            continue;
        count++;

        putsUart0("  ");
        putsUart0(nfc_table[i].name);
        putsUart0("_");
        putsUart0(nfc_table[i].group);
        putsUart0("  (");
        printUidHex(nfc_table[i].uid, nfc_table[i].uid_len);
        putsUart0(")\r\n");
    }

    if (count == 0)
        putsUart0("  (none)\r\n");
}

// process the pir and ultrasonic sensors
void processSensors(void)
{
    bool pir;
    uint16_t distance;
    char buf[8];
    char msg[32];

    // if we dont need to process sensors yet return
    if (!sensor_poll_due)
        return;
    sensor_poll_due = false;

    // ensure mqtt is connected
    if (!isMqttConnected())
        return;

    if (pir_enabled)
    {
        // publish pir sensor data if the state changes from previous state
        pir = readPir();
        if (pir != last_pir_state)
        {
            publishMqtt(TOPIC_PIR_MOTION, pir ? "on" : "off");

            if (pir_print)
            {
                putsUart0("pir: ");
                putsUart0(pir ? "on\r\n" : "off\r\n");
            }

            last_pir_state = pir;
        }
    }

    // publish distance in cm every time the pir sensor detects someone
    if (sonic_enabled && last_pir_state)
    {
        if (measureUltrasonicDistance(&distance))
        {
            snprintf(buf, sizeof(buf), "%u", distance);
            publishMqtt(TOPIC_ULT_DISTANCE, buf);

            if (sonic_print)
            {
                snprintf(msg, sizeof(msg), "sonic: %u cm\r\n", distance);
                putsUart0(msg);
            }
        }
    }
}

void processRelock(void)
{
    // if relock timer is up then send the lock message to the correct group
    if (relock_expired)
    {
        relock_expired = false;
        relock_pending = false;
        if (isMqttConnected())
            publishGroupAction(relock_group, false);
    }
}
