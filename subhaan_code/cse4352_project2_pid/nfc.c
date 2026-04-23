#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "i2c1.h"
#include "wait.h"
#include "nfc.h"

// pn532 user manual has address 0x48 we write to 0x24
// since 0x24 = 0010 0100 gets shifted left 1 to make 7
// bit address making 0100 1000 = 0x48
#define PN532_I2C_ADDRESS 0x24

// pn532 sends data using frames
// frame consists of preamble, start code, len, lcs, tfi, data, dcs, and postamble

#define PN532_PREAMBLE      0x00    // pg28 shows preamble = 00
#define PN532_STARTCODE1    0x00    // two byte start code being 00FF
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00    // postamble (after data) also 00

// tfi (frame identifier) = value depends on the way of the message
// "D4h in case of a frame from the host controller to the PN532"
// "D5h in case of a frame from the PN532 to the host controller"
#define PN532_HOST_TO_PN532 0xD4
#define PN532_PN532_TO_HOST 0xD5

// miscellaneous commands get firmware version and SAM configuration
#define PN532_COMMAND_GET_FIRMWARE      0x02    // input page 73
#define PN532_COMMAND_SAM_CONFIGURATION 0x14    // input page 14

// rf communication command + baud rate to use during init
#define PN532_COMMAND_INLIST_PASSIVE    0x4A    // input page 115
#define PN532_BAUD_ISO14443A            0x00    // same page, 106 kbps (default)

// maximum frame size we can send is limited to 255 bytes (pg 28)
// but we limit to 48 since we do not need all that space
#define PN532_FRAME_MAX 48

// default ack frame we send to indicate previous frame was received (pg 30)
const uint8_t ackFrame[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// bool so we only start communicating once initialization is done
bool nfcReady = false;

// function that waits until the pn532 is ready for communication
// will return false if it never initializes after timeout ms
bool waitForPn532Ready(uint16_t timeout_ms)
{
    uint8_t status;

    while (timeout_ms > 0)
    {
        // continuously read 1 byte status of pn532
        readI2c1Bytes(PN532_I2C_ADDRESS, &status, 1);

        // if we had an error its not ready so return false
        if (isI2c1Error())
            return false;

        // if we got the success code return true
        if (status == 0x01)
            return true;

        // if neither then pn532 not ready, wait and retry
        waitMicrosecond(1000);  // 1 ms
        timeout_ms--;
    }

    return false;
}

// read any ACK msgs we get to ensure we received everything
bool readPn532Ack(void)
{
    uint8_t ack[20];
    uint8_t i;

    waitMicrosecond(10000);

    // make sure the pn532 is actually ready
    if (!waitForPn532Ready(100))
        return false;

    // read the actual ack msg we received
    readI2c1Bytes(PN532_I2C_ADDRESS, ack, sizeof(ack));
    if (isI2c1Error())
        return false;

    // first byte is the ready byte, must be 0x01 here
    if (ack[0] != 0x01)
        return false;

    // then the rest of the bytes must be same as the ack frame
    for (i = 0; i < sizeof(ackFrame); i++)
    {
        if (ack[i + 1] != ackFrame[i])
            return false;
    }

    return true;
}

// function that actually writes all messages we send to pn532
// builds the entire frame defined in user manual
bool writePn532Command(const uint8_t *command, uint8_t length)
{
    uint8_t frame[PN532_FRAME_MAX];
    uint8_t i;
    uint8_t checksum;

    // make sure we have the correct length first of all (9 bytes, pg 28)
    // (preamble, start code, len, lcs, tfi, data, dcs, and postamble)
    if ((command == 0) || (length == 0) || ((uint8_t)(length + 9) > sizeof(frame)))
        return false;

    // now we create the actual frame
    frame[0] = 0x00;    // extra leading zeroes we add
    frame[1] = PN532_PREAMBLE;
    frame[2] = PN532_STARTCODE1;
    frame[3] = PN532_STARTCODE2;
    frame[4] = length + 1;
    // ex: LEN = 0x05, LCS = 0xFA + 1 = 0xFB, 0xFB + 0x05 = 0x100 and we only take 1 byte so 0
    frame[5] = (uint8_t)(~frame[4] + 1);    // length checksum to make sure LEN + LCS = 0
    frame[6] = PN532_HOST_TO_PN532;         // tfi

    // now we copy the data in and compute the data checksum to make sure its 0
    // data checksum is TFI + all data + DCS
    checksum = PN532_HOST_TO_PN532; // tfi
    for (i = 0; i < length; i++)
    {
        frame[7 + i] = command[i];  // write byte by byte
        checksum += command[i];     // add data for data checksum
    }

    // now finish dcs using same method to ensure its 0
    frame[7 + length] = (uint8_t)(~checksum + 1);

    // finish the frame with postamble
    frame[8 + length] = PN532_POSTAMBLE;

    // then write entire command to i2c using custom function
    writeI2c1Bytes(PN532_I2C_ADDRESS, frame, length + 9);
    if (isI2c1Error())
        return false;

    // if the command is successful we should get an ACK back so read for it
    return readPn532Ack();
}

// function reads all of the response messages we get back from the pn532
bool readPn532Response(uint8_t command, uint8_t *data, uint8_t maxLength, uint8_t *actualLength)
{
    uint8_t frame[PN532_FRAME_MAX];
    uint8_t frameLength;
    uint8_t dataLength;
    uint8_t checksum = 0;
    uint8_t i;
    uint8_t bytesToRead;

    if ((data == 0) || (actualLength == 0))
        return false;

    // total length that we need to read
    bytesToRead = maxLength + 10;

    // if the message is greater than our max frame size return false
    if (bytesToRead > sizeof(frame))
        return false;

    // wait 100ms to ensure the nfc reader is ready
    if (!waitForPn532Ready(100))
        return false;

    // read the actual bytes that we got
    readI2c1Bytes(PN532_I2C_ADDRESS, frame, bytesToRead);
    if (isI2c1Error())
        return false;

    // ensure that first byte is the ready byte = 1
    if (frame[0] != 0x01)
        return false;

    // make sure the preamble and start code is the same
    if ((frame[1] != PN532_PREAMBLE) || (frame[2] != PN532_STARTCODE1) || (frame[3] != PN532_STARTCODE2))
        return false;

    // now we check the length using checksum, should equal 0
    frameLength = frame[4];
    if ((uint8_t)(frame[4] + frame[5]) != 0)
        return false;

    // make sure the frame length is at least 2 for tfi and cmd
    if (frameLength < 2)
        return false;

    // make sure message is for us
    if (frame[6] != PN532_PN532_TO_HOST)
        return false;

    // make sure the response is command + 1 (pg 43)
    if (frame[7] != (uint8_t)(command + 1))
        return false;

    // then compute data length by subtracting tfi and cmd bytes
    dataLength = frameLength - 2;
    if (dataLength > maxLength)
        return false;

    // compute the data checksum by adding tfi, cmd, data, dcs and ensure it = 0
    for (i = 0; i < frameLength; i++)
        checksum += frame[6 + i];
    checksum += frame[6 + frameLength];
    if (checksum != 0)
        return false;

    // verify the last byte in frame is the postamble
    if (frame[7 + frameLength] != PN532_POSTAMBLE)
        return false;

    // copy the actual message data by skipping past all the frame stuff
    memcpy(data, &frame[8], dataLength);
    *actualLength = dataLength;
    return true;
}

bool initNfc(void)
{
    uint8_t response[8];
    uint8_t length;

    // first set the firmware version command
    const uint8_t firmwareCommand[] = {PN532_COMMAND_GET_FIRMWARE};

    // then set the sam config command 0x01 = normal mode, 0x14 = timeout (50 ms * 20 = 1s), irq = 0 to disable (pg89)
    const uint8_t samConfigurationCommand[] = {PN532_COMMAND_SAM_CONFIGURATION, 0x01, 0x14, 0x00};

    initI2c1();
    waitMicrosecond(500000);

    // send firmware command and get the response
    if (!writePn532Command(firmwareCommand, sizeof(firmwareCommand)))
        return false;
    if (!readPn532Response(PN532_COMMAND_GET_FIRMWARE, response, 4, &length))
        return false;
    if (length != 4)
        return false;

    // similarly send sam config command and get response
    if (!writePn532Command(samConfigurationCommand, sizeof(samConfigurationCommand)))
        return false;
    if (!readPn532Response(PN532_COMMAND_SAM_CONFIGURATION, response, 1, &length))
        return false;

    nfcReady = true;
    return true;
}

// function to actually read all of the nfc uids
// return true when it has been successfully detected and copied
bool nfcReadUid(uint8_t *uid, uint8_t *uidLength)
{
    uint8_t response[24];
    uint8_t responseLength;

    // create the polling command, set max targets to 1
    const uint8_t pollCommand[] = {PN532_COMMAND_INLIST_PASSIVE, 0x01, PN532_BAUD_ISO14443A};

    // ensure that nfc is ready and we have a valid uid
    if ((uid == 0) || (uidLength == 0) || !nfcReady)
        return false;

    *uidLength = 0;

    // actually send the poll command and read the response
    if (!writePn532Command(pollCommand, sizeof(pollCommand)))
        return false;
    if (!readPn532Response(PN532_COMMAND_INLIST_PASSIVE, response, sizeof(response), &responseLength))
        return false;

    // now we read the actual inlist passive target command output (pg 116)
    // we have number of targets found, tag number (1 for us since we only have 1 target),
    // sens_res, sel_res, uid length, uid bytes
    if (responseLength < 6)
        return false;

    // if we didnt detect anything then nothing to read
    if (response[0] == 0)
        return false;

    // make sure the uid we read isnt 0 or greater than the max length
    if ((response[5] == 0) || (response[5] > NFC_MAX_UID_LENGTH))
        return false;

    // make sure the data we received actually has correct number of bytes
    if (responseLength < (uint8_t)(6 + response[5]))
        return false;

    // copy the actual uid
    memcpy(uid, &response[6], response[5]);
    *uidLength = response[5];
    return true;
}
