#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "tm4c123gh6pm.h"
#include "spi1.h"
#include "gpio.h"
#include "wait.h"
#include "nfc.h"
#include "uart0.h"

// Names: Michael, Oscar, Subhaan

// pn532 has to write operation byte before each transaction (pg 45)
// 0x01 means we are sending a write frame, 0x03 means we are reading
#define PN532_SPI_DATAWRITE 0x01    // host writes a frame
#define PN532_SPI_DATAREAD  0x03    // host reads response frame

// pn532 sends data using frames
// frame consists of preamble, start code, len, lcs, tfi, data, dcs, and postamble
#define PN532_PREAMBLE      0x00    // pg28 shows preamble = 00
#define PN532_STARTCODE1    0x00    // two byte start code being 00FF
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00    // postamble (after data) also 00

// tfi (frame identifier) = value depends on the way of the message (pg 28)
// "D4h in case of a frame from the host controller to the PN532"
// "D5h in case of a frame from the PN532 to the host controller"
#define PN532_HOST_TO_PN532 0xD4
#define PN532_PN532_TO_HOST 0xD5

// miscellaneous commands get firmware version and SAM configuration
// firmware command to just check if we can communicate
// sam config command puts the chip in the correct mode with IRQ enabled
#define PN532_COMMAND_GET_FIRMWARE      0x02    // input page 73
#define PN532_COMMAND_SAM_CONFIGURATION 0x14    // input page 14

// rf communication commands + baud rate to use during init
// rfconfig we use to limit retries on scanning air for tags
// inlist passive is actually used to scan the tag
#define PN532_COMMAND_RFCONFIGURATION   0x32    // input page 101
#define PN532_COMMAND_INLIST_PASSIVE    0x4A    // input page 115
#define PN532_BAUD_ISO14443A            0x00    // same page, 106 kbps (default)

// rfconfiguration timing values from pg 103
#define PN532_RFCFG_MAX_RETRIES         0x05    // CfgItem = 0x05
#define MAX_RETRIES_PASSIVE_ACTIVATION  0x01    // set to 1 for two total tries

// maximum frame size we can send is limited to 255 bytes (pg 28)
// but we limit to 48 since we do not need all that space
#define PN532_FRAME_MAX 48

// cs is on pd1 same as spi1 fss but we drive it manually
// since pn532 needs cs held low across the whole frame
#define CS_PORT PORTD
#define CS_PIN  1

// irq pin from pn532, goes low when it has data ready for us
#define IRQ_PORT PORTA
#define IRQ_PIN  7

// how often we check the irq pin and how long we wait total
// we will wait 5 ms between checks and 200ms total for 2 tries
#define IRQ_POLL_INTERVAL_MS 5
#define IRQ_TIMEOUT_MS       200

// default ack frame we send to indicate previous frame was received (pg 30)
const uint8_t ackFrame[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// bool so we only start communicating once initialization is done
bool nfcReady = false;

// reverses bit order in a byte since spi sends msb first
// but the pn532 reads/sends lsb first so we flip to match
uint8_t reverse_bits(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

// set cs low to start sending data frames
void cs_low(void)
{
    setPinValue(CS_PORT, CS_PIN, 0);
}

// set cs high to stop sending data frames
void cs_high(void)
{
    setPinValue(CS_PORT, CS_PIN, 1);
}

// send one byte to pn532 with bit-reversal
// then reads rx fifo to clear it
void spi_send_byte(uint8_t b)
{
    writeSpi1Data(reverse_bits(b));
    (void)readSpi1Data();
}

// read one byte from pn532 with bit reversal
// we send a dummy 0 so the clock runs while we shift the data in
uint8_t spi_recv_byte(void)
{
    writeSpi1Data(0x00);
    return reverse_bits((uint8_t)readSpi1Data());
}

// check if pn532 irq pin is low meaning it has data ready for us
// goes low when chip has frame waiting, back high after we read it
bool is_pn532_ready(void)
{
    return getPinValue(IRQ_PORT, IRQ_PIN) == 0;
}

// function that waits until the pn532 is ready for communication
// by checking irq pin says it is ready; will return false
// if it never initializes after timeout ms
bool waitForPn532Ready(uint16_t timeout_ms)
{
    uint16_t elapsed = 0;

    while (elapsed < timeout_ms)
    {
        // continuously check if irq pin says its ready
        if (is_pn532_ready())
            return true;

        // if not ready yet wait and retry
        waitMicrosecond(IRQ_POLL_INTERVAL_MS * 1000);
        elapsed += IRQ_POLL_INTERVAL_MS;
    }

    return false;
}

// read any ACK msgs we get to ensure we received everything
bool readPn532Ack(void)
{
    uint8_t ack[6];
    uint8_t i;

    // make sure the pn532 irq pin is actually ready
    if (!waitForPn532Ready(IRQ_TIMEOUT_MS))
        return false;

    // first pull cs low to send/receive data
    cs_low();
    waitMicrosecond(1000);

    // then send first byte to initiate data read
    spi_send_byte(PN532_SPI_DATAREAD);

    // afterwards start reading and decoding the msg we received
    for (i = 0; i < sizeof(ackFrame); i++)
        ack[i] = spi_recv_byte();

    // pull cs back high to end reading
    cs_high();

    // check if the ACK msg we received is the correct ACK frame
    for (i = 0; i < sizeof(ackFrame); i++)
    {
        if (ack[i] != ackFrame[i])
            return false;
    }

    return true;
}

// function that actually writes all messages we send to pn532
// builds the entire frame defined in user manual
bool writePn532Command(const uint8_t *command, uint8_t length)
{
    uint8_t frame[PN532_FRAME_MAX];
    uint8_t frameLength;
    uint8_t i;
    uint8_t checksum;

    // make sure we have the correct length first of all (8 byte + data, pg 28)
    // (preamble, start code, len, lcs, tfi, data, dcs, and postamble)
    if ((command == 0) || (length == 0) || ((uint8_t)(length + 8) > sizeof(frame)))
        return false;

    // now we create the actual frame
    frame[0] = PN532_PREAMBLE;
    frame[1] = PN532_STARTCODE1;
    frame[2] = PN532_STARTCODE2;
    frame[3] = length + 1;                          // len = data + tfi
    // ex: LEN = 0x05, LCS = 0xFA + 1 = 0xFB, 0xFB + 0x05 = 0x100 and we only take 1 byte so 0
    frame[4] = (uint8_t)(~frame[3] + 1);            // length checksum to make sure LEN + LCS = 0
    frame[5] = PN532_HOST_TO_PN532;                 // tfi

    // now we copy the data in and compute the data checksum to make sure its 0
    // data checksum is TFI + all data + DCS
    checksum = PN532_HOST_TO_PN532; // tfi
    for (i = 0; i < length; i++)
    {
        frame[6 + i] = command[i];  // write byte by byte
        checksum += command[i];     // add data for data checksum
    }

    // now finish dcs using same method to ensure its 0
    frame[6 + length] = (uint8_t)(~checksum + 1);

    // finish the frame with postamble
    frame[7 + length] = PN532_POSTAMBLE;

    // add up whole frame header + data
    frameLength = 8 + length;

    // actually send the command by pulling CS low
    // and sending the whole frame, then pulling it back high
    cs_low();
    waitMicrosecond(2000);
    spi_send_byte(PN532_SPI_DATAWRITE);
    for (i = 0; i < frameLength; i++)
        spi_send_byte(frame[i]);
    cs_high();

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
    bytesToRead = maxLength + 9;

    // if the message is greater than our max frame size return false
    if (bytesToRead > sizeof(frame))
        return false;

    // wait for irq to drop indicating response is ready
    if (!waitForPn532Ready(IRQ_TIMEOUT_MS))
        return false;

    // since we know IRQ is read pull CS low and read data
    cs_low();
    waitMicrosecond(1000);
    spi_send_byte(PN532_SPI_DATAREAD);
    for (i = 0; i < bytesToRead; i++)
        frame[i] = spi_recv_byte();
    cs_high();

    // make sure the preamble and start code is the same
    if ((frame[0] != PN532_PREAMBLE) || (frame[1] != PN532_STARTCODE1) || (frame[2] != PN532_STARTCODE2))
        return false;

    // now we check the length using checksum, should equal 0
    frameLength = frame[3];
    if ((uint8_t)(frame[3] + frame[4]) != 0)
        return false;

    // make sure the frame length is at least 2 for tfi and cmd
    if (frameLength < 2)
        return false;

    // make sure message is for us
    if (frame[5] != PN532_PN532_TO_HOST)
        return false;


    // make sure the response is command + 1 (pg 43)
    if (frame[6] != (uint8_t)(command + 1))
        return false;

    // then compute data length by subtracting tfi and cmd bytes
    dataLength = frameLength - 2;
    if (dataLength > maxLength)
        return false;

    // compute the data checksum by adding tfi, cmd, data, dcs and ensure it = 0
    for (i = 0; i < frameLength; i++)
        checksum += frame[5 + i];
    checksum += frame[5 + frameLength];
    if (checksum != 0)
        return false;

    // verify the last byte in frame is the postamble
    if (frame[6 + frameLength] != PN532_POSTAMBLE)
        return false;

    // copy the actual message data by skipping past all the frame stuff
    memcpy(data, &frame[7], dataLength);
    *actualLength = dataLength;
    return true;
}

bool initNfc(void)
{
    uint8_t response[8];
    uint8_t length;

    // first set the firmware version command
    const uint8_t firmwareCommand[] = {PN532_COMMAND_GET_FIRMWARE};

    // then set the sam config command 0x01 = normal mode, 0x14 = timeout (50 ms * 20 = 1s), irq = 0x01 to enable (pg89)
    const uint8_t samConfigurationCommand[] = {PN532_COMMAND_SAM_CONFIGURATION, 0x01, 0x14, 0x01};

    // then set the rfconfiguration (pg 101)
    // we set CfgItem to 0x05 for pg 103, then leave the first 2 bytes as default
    // then we set the max retries passive action to 1 to have 2 total retries
    const uint8_t rfConfigCommand[] = {
        PN532_COMMAND_RFCONFIGURATION,
        PN532_RFCFG_MAX_RETRIES,            // 0x05 = max retries config
        0xFF,                               // default atr retires
        0x01,                               // default psl retries
        MAX_RETRIES_PASSIVE_ACTIVATION      // 0x01 = max 2 attempts when scanning for tag
    };

    // set the flag to false if we reinit
    nfcReady = false;

    putsUart0("nfc: init spi1\r\n");

    // initialize spi1 with no fss since we will manually drive it
    initSpi1(USE_SSI_RX);
    setSpi1BaudRate(1000000, 40000000);
    setSpi1Mode(0, 0);

    // override the 16 bit data frames by setting it to 8 bit data frames for pn532
    SSI1_CR1_R &= ~SSI_CR1_SSE;
    SSI1_CR0_R = (SSI1_CR0_R & ~SSI_CR0_DSS_M) | SSI_CR0_DSS_8;
    SSI1_CR1_R |= SSI_CR1_SSE;

    // clear any stale bytes in receive fifo
    while (SSI1_SR_R & SSI_SR_RNE)
        (void)SSI1_DR_R;

    // configure irq input on PA7 and enable pullup
    enablePort(IRQ_PORT);
    selectPinDigitalInput(IRQ_PORT, IRQ_PIN);
    enablePinPullup(IRQ_PORT, IRQ_PIN);

    // set CS to high before doing anything
    cs_high();
    waitMicrosecond(1000);

    // pulse CS to ensure the line is active
    cs_low();
    waitMicrosecond(2000);
    cs_high();
    waitMicrosecond(2000);

    // send firmware command and get the response to ensure commands work
    putsUart0("nfc: send firmware cmd\r\n");
    if (!writePn532Command(firmwareCommand, sizeof(firmwareCommand)))
    {
        putsUart0("nfc: firmware cmd failed\r\n");
        return false;
    }
    if (!readPn532Response(PN532_COMMAND_GET_FIRMWARE, response, 4, &length) || length != 4)
    {
        putsUart0("nfc: firmware response failed\r\n");
        return false;
    }

    // similarly send sam config command and get response
    putsUart0("nfc: send sam config\r\n");
    if (!writePn532Command(samConfigurationCommand, sizeof(samConfigurationCommand)))
    {
        putsUart0("nfc: sam cmd failed\r\n");
        return false;
    }
    if (!readPn532Response(PN532_COMMAND_SAM_CONFIGURATION, response, 1, &length))
    {
        putsUart0("nfc: sam response failed\r\n");
        return false;
    }

    // send the rf config command and get the response
    putsUart0("nfc: send rf config\r\n");
    if (!writePn532Command(rfConfigCommand, sizeof(rfConfigCommand)))
    {
        putsUart0("nfc: rf config cmd failed\r\n");
        return false;
    }
    if (!readPn532Response(PN532_COMMAND_RFCONFIGURATION, response, 1, &length))
    {
        putsUart0("nfc: rf config response failed\r\n");
        return false;
    }

    // once all commands have run successfully, nfc has initialized
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
