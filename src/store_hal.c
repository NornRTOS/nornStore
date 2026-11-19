/**
 * @file store_hal.c
 * The store on nornCore's flash HAL (hal/flash.h): the driver table and store_initHal().
 * Compiled on the AVR and the Xtensa only; the host tests bring their own driver. Linked
 * only when store_initHal() or store_halFlash is referenced.
 */

#include "store.h"

#if defined(__AVR__)  ||  defined(__XTENSA__)
#include "flash.h"

const store_flash_t store_halFlash =
{
    hal_flashRead, hal_flashWrite, hal_flashEraseBlock,
    HAL_FLASH_BLOCK_SIZE, HAL_FLASH_BLOCKS, HAL_FLASH_WRITE_ALIGN
};

int store_initHal(store_obj_t *pObjs, uint8_t count, uint8_t *pBuf, size_t bufSize)
{
    if(!hal_flashInit())
        return STORE_ERR_FLASH;
    return store_init(&store_halFlash, pObjs, count, pBuf, bufSize);
}
#else
typedef int store_hal_unused_t;
#endif
