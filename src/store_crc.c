/**
 * @file store_crc.c
 * CRC-16/CCITT-FALSE, bit by bit: about 40 bytes of code and no table. The blocks are
 * small and a save is rare, so speed does not matter here.
 */

#include "store_internal.h"

uint16_t store_crc16(uint16_t crc, const uint8_t *p, size_t n)
{
    while(n-- > 0)
    {
        uint8_t i;
        crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)*p++ << 8));
        for(i = 0; i < 8; ++i)
            crc = (crc & 0x8000u) ? (uint16_t)(((unsigned)crc << 1) ^ 0x1021u) : (uint16_t)((unsigned)crc << 1);
    }
    return crc;
}
