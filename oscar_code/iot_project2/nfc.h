#ifndef NFC_H_
#define NFC_H_

#include <stdint.h>
#include <stdbool.h>

#define NFC_MAX_UID_LENGTH 10

bool initNfc(void);
bool nfcReadUid(uint8_t *uid, uint8_t *uidLength);

#endif
