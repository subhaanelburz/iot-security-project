// PN532 NFC Library

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "tm4c123gh6pm.h"
#include "gpio.h"
#include "wait.h"
#include "nfc.h"

// Pins
#define NFC_SCL PORTA,6
#define NFC_SDA PORTA,7

// PN532 constants
#define PN532_I2C_ADDRESS                 0x24
#define PN532_PREAMBLE                    0x00
#define PN532_STARTCODE1                  0x00
#define PN532_STARTCODE2                  0xFF
#define PN532_POSTAMBLE                   0x00
#define PN532_HOST_TO_PN532               0xD4
#define PN532_PN532_TO_HOST               0xD5
#define PN532_COMMAND_GET_FIRMWARE        0x02
#define PN532_COMMAND_SAM_CONFIGURATION   0x14
#define PN532_COMMAND_INLIST_PASSIVE      0x4A
#define PN532_BAUD_ISO14443A              0x00

#define I2C_STANDARD_100KHZ_TPR           19
#define I2C_TIMEOUT                       100000
#define PN532_FRAME_MAX                   48

static bool nfcReady = false;

static const uint8_t ackFrame[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

static bool waitForI2c1BusIdle(void)
{
    uint32_t timeout = I2C_TIMEOUT;
    while ((I2C1_MCS_R & I2C_MCS_BUSBSY) && timeout > 0)
        timeout--;
    return timeout > 0;
}

static bool waitForI2c1Complete(void)
{
    uint32_t timeout = I2C_TIMEOUT;
    while ((I2C1_MCS_R & I2C_MCS_BUSY) && timeout > 0)
        timeout--;
    if (timeout == 0)
        return false;
    if (I2C1_MCS_R & (I2C_MCS_ERROR | I2C_MCS_ARBLST | I2C_MCS_CLKTO))
        return false;
    if (I2C1_MCS_R & (I2C_MCS_ADRACK | I2C_MCS_DATACK))
        return false;
    return true;
}

static bool writeI2c1(const uint8_t *data, uint8_t length)
{
    uint8_t i;

    if ((data == 0) || (length == 0))
        return false;

    if (!waitForI2c1BusIdle())
        return false;

    I2C1_MSA_R = PN532_I2C_ADDRESS << 1;
    I2C1_MDR_R = data[0];
    I2C1_MCS_R = (length == 1) ? (I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP)
                               : (I2C_MCS_START | I2C_MCS_RUN);
    if (!waitForI2c1Complete())
        return false;

    for (i = 1; i < length; i++)
    {
        I2C1_MDR_R = data[i];
        I2C1_MCS_R = (i == (length - 1)) ? (I2C_MCS_RUN | I2C_MCS_STOP) : I2C_MCS_RUN;
        if (!waitForI2c1Complete())
            return false;
    }

    return true;
}

static bool readI2c1(uint8_t *data, uint8_t length)
{
    uint8_t i;

    if ((data == 0) || (length == 0))
        return false;

    if (!waitForI2c1BusIdle())
        return false;

    I2C1_MSA_R = (PN532_I2C_ADDRESS << 1) | I2C_MSA_RS;

    if (length == 1)
    {
        I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_STOP;
        if (!waitForI2c1Complete())
            return false;
        data[0] = I2C1_MDR_R & 0xFF;
        return true;
    }

    I2C1_MCS_R = I2C_MCS_START | I2C_MCS_RUN | I2C_MCS_ACK;
    if (!waitForI2c1Complete())
        return false;
    data[0] = I2C1_MDR_R & 0xFF;

    for (i = 1; i < length; i++)
    {
        I2C1_MCS_R = (i == (length - 1)) ? (I2C_MCS_RUN | I2C_MCS_STOP)
                                         : (I2C_MCS_RUN | I2C_MCS_ACK);
        if (!waitForI2c1Complete())
            return false;
        data[i] = I2C1_MDR_R & 0xFF;
    }

    return true;
}

static void initI2c1(void)
{
    SYSCTL_RCGCI2C_R |= SYSCTL_RCGCI2C_R1;
    _delay_cycles(3);
    enablePort(PORTA);

    selectPinPushPullOutput(NFC_SCL);
    setPinAuxFunction(NFC_SCL, GPIO_PCTL_PA6_I2C1SCL);
    enablePinPullup(NFC_SCL);

    selectPinOpenDrainOutput(NFC_SDA);
    setPinAuxFunction(NFC_SDA, GPIO_PCTL_PA7_I2C1SDA);
    enablePinPullup(NFC_SDA);

    I2C1_MCR_R = I2C_MCR_MFE;
    I2C1_MTPR_R = I2C_STANDARD_100KHZ_TPR;
}

static bool waitForPn532Ready(uint16_t timeoutMs)
{
    uint8_t status;

    while (timeoutMs > 0)
    {
        if (readI2c1(&status, 1) && (status == 0x01))
            return true;
        waitMicrosecond(1000);
        timeoutMs--;
    }

    return false;
}

static bool readPn532Ack(void)
{
    uint8_t ack[1 + sizeof(ackFrame)];
    uint8_t i;

    if (!waitForPn532Ready(50))
        return false;

    if (!readI2c1(ack, sizeof(ack)))
        return false;

    if (ack[0] != 0x01)
        return false;

    for (i = 0; i < sizeof(ackFrame); i++)
    {
        if (ack[i + 1] != ackFrame[i])
            return false;
    }

    return true;
}

static bool writePn532Command(const uint8_t *command, uint8_t length)
{
    uint8_t frame[PN532_FRAME_MAX];
    uint8_t i;
    uint8_t checksum;

    if ((command == 0) || (length == 0) || ((uint8_t)(length + 9) > sizeof(frame)))
        return false;

    frame[0] = 0x00;
    frame[1] = PN532_PREAMBLE;
    frame[2] = PN532_STARTCODE1;
    frame[3] = PN532_STARTCODE2;
    frame[4] = length + 1;
    frame[5] = (uint8_t)(~frame[4] + 1);
    frame[6] = PN532_HOST_TO_PN532;

    checksum = PN532_HOST_TO_PN532;
    for (i = 0; i < length; i++)
    {
        frame[7 + i] = command[i];
        checksum += command[i];
    }

    frame[7 + length] = (uint8_t)(~checksum + 1);
    frame[8 + length] = PN532_POSTAMBLE;

    if (!writeI2c1(frame, length + 9))
        return false;

    return readPn532Ack();
}

static bool readPn532Response(uint8_t command, uint8_t *data, uint8_t maxLength, uint8_t *actualLength)
{
    uint8_t frame[PN532_FRAME_MAX];
    uint8_t frameLength;
    uint8_t dataLength;
    uint8_t checksum = 0;
    uint8_t i;
    uint8_t bytesToRead;

    if ((data == 0) || (actualLength == 0))
        return false;

    bytesToRead = maxLength + 10;
    if (bytesToRead > sizeof(frame))
        return false;

    if (!waitForPn532Ready(100))
        return false;

    if (!readI2c1(frame, bytesToRead))
        return false;

    if (frame[0] != 0x01)
        return false;
    if ((frame[1] != PN532_PREAMBLE) || (frame[2] != PN532_STARTCODE1) || (frame[3] != PN532_STARTCODE2))
        return false;

    frameLength = frame[4];
    if ((uint8_t)(frame[4] + frame[5]) != 0)
        return false;
    if (frameLength < 2)
        return false;
    if (frame[6] != PN532_PN532_TO_HOST)
        return false;
    if (frame[7] != (uint8_t)(command + 1))
        return false;

    dataLength = frameLength - 2;
    if (dataLength > maxLength)
        return false;

    for (i = 0; i < frameLength; i++)
        checksum += frame[6 + i];
    checksum += frame[6 + frameLength];
    if (checksum != 0)
        return false;

    if (frame[7 + frameLength] != PN532_POSTAMBLE)
        return false;

    memcpy(data, &frame[8], dataLength);
    *actualLength = dataLength;
    return true;
}

bool initNfc(void)
{
    uint8_t response[8];
    uint8_t length;
    const uint8_t firmwareCommand[] = {PN532_COMMAND_GET_FIRMWARE};
    const uint8_t samConfigurationCommand[] = {PN532_COMMAND_SAM_CONFIGURATION, 0x01, 0x14, 0x00};

    initI2c1();
    waitMicrosecond(100000);

    if (!writePn532Command(firmwareCommand, sizeof(firmwareCommand)))
        return false;
    if (!readPn532Response(PN532_COMMAND_GET_FIRMWARE, response, 4, &length))
        return false;
    if (length != 4)
        return false;

    if (!writePn532Command(samConfigurationCommand, sizeof(samConfigurationCommand)))
        return false;
    if (!readPn532Response(PN532_COMMAND_SAM_CONFIGURATION, response, 1, &length))
        return false;

    nfcReady = true;
    return true;
}

bool nfcReadUid(uint8_t *uid, uint8_t *uidLength)
{
    uint8_t response[24];
    uint8_t responseLength;
    const uint8_t pollCommand[] = {PN532_COMMAND_INLIST_PASSIVE, 0x01, PN532_BAUD_ISO14443A};

    if ((uid == 0) || (uidLength == 0) || !nfcReady)
        return false;

    *uidLength = 0;

    if (!writePn532Command(pollCommand, sizeof(pollCommand)))
        return false;

    if (!readPn532Response(PN532_COMMAND_INLIST_PASSIVE, response, sizeof(response), &responseLength))
        return false;

    if (responseLength < 6)
        return false;

    if (response[0] == 0)
        return false;

    if ((response[5] == 0) || (response[5] > NFC_MAX_UID_LENGTH))
        return false;

    if (responseLength < (uint8_t)(6 + response[5]))
        return false;

    memcpy(uid, &response[6], response[5]);
    *uidLength = response[5];
    return true;
}
