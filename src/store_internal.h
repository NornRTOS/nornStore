#ifndef STORE_INTERNAL_INCLUDED
#define STORE_INTERNAL_INCLUDED
/**
 * @file store_internal.h
 * Shared between the source files of nornStore; not for applications.
 */

#include "store.h"

/** CRC-16/CCITT-FALSE (polynomial 0x1021, initial 0xFFFF), continued from @p crc. */
uint16_t store_crc16(uint16_t crc, const uint8_t *p, size_t n);

/** Block header, as laid out in the block (12 bytes, little-endian). */
#define STORE_MAGIC         0x3174536Eul    /* 'nSt1' read as a little-endian word */

#endif  /* STORE_INTERNAL_INCLUDED */
