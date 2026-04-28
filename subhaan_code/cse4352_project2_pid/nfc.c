#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "tm4c123gh6pm.h"
#include "spi1.h"
#include "gpio.h"
#include "wait.h"
#include "nfc.h"
#include "uart0.h"

// pn532 spi prefix bytes from user manual sect 6.2.5
// these are the logical values, we bit-reverse before tx since
// pn532 is lsb-first but tm4c ssi hw is msb-first only
#define PN532_SPI_DATAWRITE 0x01    // host writes a frame
#define PN532_SPI_DATAREAD  0x03    // host reads response frame

// pn532 frame layout, same as i2c version
#define PN532_PREAMBLE      0x00    // pg28 preamble = 00
#define PN532_STARTCODE1    0x00    // two byte start code 00FF
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00    // postamble also 00

// tfi value depends on direction (pg29)
#define PN532_HOST_TO_PN532 0xD4
#define PN532_PN532_TO_HOST 0xD5

// pn532 commands
#define PN532_COMMAND_GET_FIRMWARE      0x02    // pg73
#define PN532_COMMAND_SAM_CONFIGURATION 0x14    // pg89
#define PN532_COMMAND_RFCONFIGURATION   0x32    // pg101
#define PN532_COMMAND_INLIST_PASSIVE    0x4A    // pg115
#define PN532_BAUD_ISO14443A            0x00    // 106 kbps default

// rfconfiguration cfgitem 5 = max retries (pg103)
// configdata is 3 bytes: mxrtyatr, mxrtypsl, mxrtypassiveactivation
// default is 0xff 0x01 0xff which makes inlistpassive retry forever
// 0x01 means try twice (~60ms total) which is enough to catch a tap
#define PN532_RFCFG_MAX_RETRIES         0x05
#define MAX_RETRIES_PASSIVE_ACTIVATION  0x01

// max frame size we support, plenty for our use case
#define PN532_FRAME_MAX 48

// cs is on pd1 same as ssi1fss but driven manually
// since pn532 needs cs held low across the whole frame
#define CS_PORT PORTD
#define CS_PIN  1

// irq from pn532, active low open drain (needs pullup)
// pa7 is free since we dropped i2c1, easy to solder on the launchpad
#define IRQ_PORT PORTA
#define IRQ_PIN  7

// timeouts and intervals for ready waits
// since rfconfiguration limits passive activation to 2 tries (~60ms),
// 200 ms is plenty as a safety net without blocking the main loop too long
#define IRQ_POLL_INTERVAL_MS 5
#define IRQ_TIMEOUT_MS       200

// expected ack frame from pn532 after every host command (pg30)
const uint8_t ackFrame[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// flag so other code only talks to pn532 once init is done
bool nfcReady = false;

// reverse bit order in a byte
// pn532 spi is lsb-first but tm4c ssi shifts msb-first
// so we flip every byte going in and coming out
uint8_t reverse_bits(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

// drive cs low to start a transaction
void cs_low(void)
{
    setPinValue(CS_PORT, CS_PIN, 0);
}

// drive cs high to end a transaction
void cs_high(void)
{
    setPinValue(CS_PORT, CS_PIN, 1);
}

// send one byte to pn532 with bit-reversal
// also drains rx fifo since every spi tx generates an rx byte
void spi_send_byte(uint8_t b)
{
    writeSpi1Data(reverse_bits(b));
    (void)readSpi1Data();
}

// receive one byte from pn532 with bit-reversal
// clocks out a dummy 0 so sclk runs while we shift data in
uint8_t spi_recv_byte(void)
{
    writeSpi1Data(0x00);
    return reverse_bits((uint8_t)readSpi1Data());
}

// pn532 irq pin is active low
// goes low when chip has a frame waiting for us, returns high after we read it
bool is_pn532_ready(void)
{
    return getPinValue(IRQ_PORT, IRQ_PIN) == 0;
}

// poll the irq pin until pn532 signals data ready, or timeout
// no spi traffic during the wait, just a cheap pin read every few ms
bool waitForPn532Ready(uint16_t timeout_ms)
{
    uint16_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        if (is_pn532_ready())
            return true;
        waitMicrosecond(IRQ_POLL_INTERVAL_MS * 1000);
        elapsed += IRQ_POLL_INTERVAL_MS;
    }
    return false;
}

// read and verify the ack frame pn532 sends after every command
bool readPn532Ack(void)
{
    uint8_t ack[6];
    uint8_t i;

    // wait for irq to drop indicating ack is ready
    if (!waitForPn532Ready(IRQ_TIMEOUT_MS))
        return false;

    // pull the ack frame using a dataread transaction
    // reading the frame causes pn532 to release irq back to high
    cs_low();
    waitMicrosecond(1000);
    spi_send_byte(PN532_SPI_DATAREAD);
    for (i = 0; i < sizeof(ackFrame); i++)
        ack[i] = spi_recv_byte();
    cs_high();

    // compare against the known good ack pattern
    for (i = 0; i < sizeof(ackFrame); i++)
    {
        if (ack[i] != ackFrame[i])
            return false;
    }
    return true;
}

// build and send a pn532 command frame over spi
// note: the spi frame has no leading 0x00 like the i2c version did
// the datawrite prefix byte replaces that role
bool writePn532Command(const uint8_t *command, uint8_t length)
{
    uint8_t frame[PN532_FRAME_MAX];
    uint8_t frameLength;
    uint8_t i;
    uint8_t checksum;

    // total frame size is 8 bytes overhead + data length (pg28)
    if ((command == 0) || (length == 0) || ((uint8_t)(length + 8) > sizeof(frame)))
        return false;

    // build the frame
    frame[0] = PN532_PREAMBLE;
    frame[1] = PN532_STARTCODE1;
    frame[2] = PN532_STARTCODE2;
    frame[3] = length + 1;                          // len = data + tfi
    frame[4] = (uint8_t)(~frame[3] + 1);            // lcs so len + lcs = 0
    frame[5] = PN532_HOST_TO_PN532;                 // tfi

    // copy data and accumulate checksum at the same time
    // dcs covers tfi + all data bytes
    checksum = PN532_HOST_TO_PN532;
    for (i = 0; i < length; i++)
    {
        frame[6 + i] = command[i];
        checksum += command[i];
    }

    frame[6 + length] = (uint8_t)(~checksum + 1);   // dcs
    frame[7 + length] = PN532_POSTAMBLE;
    frameLength = 8 + length;

    // send: cs low, datawrite prefix, frame bytes, cs high
    // 2ms before clocking gives pn532 time to wake from idle
    cs_low();
    waitMicrosecond(2000);
    spi_send_byte(PN532_SPI_DATAWRITE);
    for (i = 0; i < frameLength; i++)
        spi_send_byte(frame[i]);
    cs_high();

    // wait for irq + verify ack frame
    return readPn532Ack();
}

// read a response frame from pn532 over spi
// validates all framing fields and copies the data payload out
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

    // 9 bytes of framing overhead (no leading ready byte unlike i2c)
    // preamble + start1 + start2 + len + lcs + tfi + cmd + dcs + postamble
    bytesToRead = maxLength + 9;
    if (bytesToRead > sizeof(frame))
        return false;

    // wait for irq to drop indicating response is ready
    if (!waitForPn532Ready(IRQ_TIMEOUT_MS))
        return false;

    // pull the response frame
    cs_low();
    waitMicrosecond(1000);
    spi_send_byte(PN532_SPI_DATAREAD);
    for (i = 0; i < bytesToRead; i++)
        frame[i] = spi_recv_byte();
    cs_high();

    // verify preamble and start code at offsets 0,1,2 (no ready byte in spi)
    if ((frame[0] != PN532_PREAMBLE) || (frame[1] != PN532_STARTCODE1) || (frame[2] != PN532_STARTCODE2))
        return false;

    // length checksum: len + lcs must equal 0
    frameLength = frame[3];
    if ((uint8_t)(frame[3] + frame[4]) != 0)
        return false;
    if (frameLength < 2)
        return false;

    // tfi must indicate pn532 to host
    if (frame[5] != PN532_PN532_TO_HOST)
        return false;
    // response cmd byte is request cmd + 1 (pg43)
    if (frame[6] != (uint8_t)(command + 1))
        return false;

    dataLength = frameLength - 2;       // subtract tfi and cmd bytes
    if (dataLength > maxLength)
        return false;

    // data checksum: tfi + cmd + data + dcs must equal 0
    for (i = 0; i < frameLength; i++)
        checksum += frame[5 + i];
    checksum += frame[5 + frameLength];
    if (checksum != 0)
        return false;

    // postamble check
    if (frame[6 + frameLength] != PN532_POSTAMBLE)
        return false;

    // copy out the actual payload, skipping the framing
    memcpy(data, &frame[7], dataLength);
    *actualLength = dataLength;
    return true;
}

bool initNfc(void)
{
    uint8_t response[8];
    uint8_t length;

    // get firmware version, takes no args
    const uint8_t firmwareCommand[] = {PN532_COMMAND_GET_FIRMWARE};

    // sam config: 0x01 normal mode, 0x14 timeout (50ms * 20 = 1s), 0x01 use irq line
    const uint8_t samConfigurationCommand[] = {PN532_COMMAND_SAM_CONFIGURATION, 0x01, 0x14, 0x01};

    // rfconfiguration to limit passive activation retries
    // without this, inlistpassive blocks until a tag appears (forever) and
    // starves the main loop, breaking tcp/mqtt timing
    // mxrtyatr 0xff and mxrtypsl 0x01 are the defaults, only the last byte changes
    const uint8_t rfConfigCommand[] = {
        PN532_COMMAND_RFCONFIGURATION,
        PN532_RFCFG_MAX_RETRIES,
        0xFF,                               // mxrtyatr default
        0x01,                               // mxrtypsl default
        MAX_RETRIES_PASSIVE_ACTIVATION      // mxrtypassiveactivation, the one we care about
    };

    // clear so a failed reinit does not leave a stale flag
    nfcReady = false;

    putsUart0("nfc: init spi1\r\n");

    // bring up spi1, no fss because we drive cs by hand
    initSpi1(USE_SSI_RX);
    setSpi1BaudRate(1000000, 40000000);     // 1 mhz, well under pn532 5 mhz max
    setSpi1Mode(0, 0);                      // pn532 spi is mode 0 (cpol 0 cpha 0)

    // override 16 bit dss from spi1.c init since pn532 is byte oriented
    SSI1_CR1_R &= ~SSI_CR1_SSE;
    SSI1_CR0_R = (SSI1_CR0_R & ~SSI_CR0_DSS_M) | SSI_CR0_DSS_8;
    SSI1_CR1_R |= SSI_CR1_SSE;

    // drain any stale rx bytes left in the fifo from configuration
    while (SSI1_SR_R & SSI_SR_RNE)
        (void)SSI1_DR_R;

    // configure irq input on pe0, pullup since pn532 irq is open drain
    enablePort(IRQ_PORT);
    selectPinDigitalInput(IRQ_PORT, IRQ_PIN);
    enablePinPullup(IRQ_PORT, IRQ_PIN);

    // park cs high (idle) before first transaction
    cs_high();
    waitMicrosecond(1000);

    // wakeup pulse, lets pn532 come out of low power state
    cs_low();
    waitMicrosecond(2000);
    cs_high();
    waitMicrosecond(2000);

    // ask for firmware version, this also confirms basic comms work
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

    // configure sam, normal mode with irq line enabled
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

    // limit how long inlistpassive blocks when no tag is present
    // this is the key fix for main loop responsiveness
    putsUart0("nfc: send rf config\r\n");
    if (!writePn532Command(rfConfigCommand, sizeof(rfConfigCommand)))
    {
        putsUart0("nfc: rf config cmd failed\r\n");
        return false;
    }
    // rfconfiguration response is just the cmd echo with no payload
    if (!readPn532Response(PN532_COMMAND_RFCONFIGURATION, response, 1, &length))
    {
        putsUart0("nfc: rf config response failed\r\n");
        return false;
    }

    nfcReady = true;
    return true;
}

// poll for one passive iso14443a target, return its uid
// returns true only when a tag is detected and the uid was copied out
// with rfconfiguration set, this returns in ~60ms when no tag is present
bool nfcReadUid(uint8_t *uid, uint8_t *uidLength)
{
    uint8_t response[24];
    uint8_t responseLength;

    // inlist passive: max 1 target, default baud
    const uint8_t pollCommand[] = {PN532_COMMAND_INLIST_PASSIVE, 0x01, PN532_BAUD_ISO14443A};

    if ((uid == 0) || (uidLength == 0) || !nfcReady)
        return false;

    *uidLength = 0;

    if (!writePn532Command(pollCommand, sizeof(pollCommand)))
        return false;
    if (!readPn532Response(PN532_COMMAND_INLIST_PASSIVE, response, sizeof(response), &responseLength))
        return false;

    // response layout (pg116):
    // [0] = num targets, [1] = tag id, [2-3] = sens_res, [4] = sel_res,
    // [5] = uid length, [6..6+uidLen-1] = uid bytes
    if (responseLength < 6)
        return false;
    if (response[0] == 0)               // no target detected
        return false;
    if ((response[5] == 0) || (response[5] > NFC_MAX_UID_LENGTH))
        return false;
    if (responseLength < (uint8_t)(6 + response[5]))
        return false;

    memcpy(uid, &response[6], response[5]);
    *uidLength = response[5];
    return true;
}
