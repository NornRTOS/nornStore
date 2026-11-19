#ifndef STORE_INCLUDED
#define STORE_INCLUDED
/**
 * @file store.h
 * @brief nornStore: persistent configuration objects for nornRTOS applications.
 *
 * A small set of typed objects (a hostname, an IP address, a mode, a calibration) kept
 * in RAM while the system runs and saved as a whole to two blocks of flash or EEPROM in
 * turn: every save writes the inactive block and makes it valid last, so a power failure
 * at any point leaves exactly one valid image, the old or the new. nornStore is an
 * optional module on nornCore's public API: an application that does not call
 * store_init() gets no byte of it, and the kernel is not changed for it.
 *
 * @par What the application owns
 * Everything that costs memory: the object table (one store_obj_t per object, with its
 * id, type and room), the RAM buffer the values live in, the flash driver
 * (store_flash_t; nornCore's hal/flash.h has one per architecture), and the names, if a
 * console wants them. nornStore keeps a few pointers and a mutex.
 *
 * @par Objects
 * An object is a 16 bit id the application defines, a type (STORE_U8 .. STORE_BLOB), a
 * version byte the application interprets, and up to 255 bytes of data. The store knows
 * no names: `system.hostname` is a constant in the application's table, and the console
 * command gets the names the same way nosh gets task names.
 *
 * @par Context
 * store_init() is called from main() before rtos_start(). The other functions are for
 * task context; before rtos_start() they may be called from main() as well (one context,
 * nothing to lock). Not from an interrupt, not from the idle task. store_save() blocks
 * the calling task for the flash operation: tens of milliseconds on an ESP32 with
 * interrupts masked while a sector is erased (see hal/flash.h), up to a second on an
 * AVR with interrupts enabled. It is a maintenance operation, done when the device is
 * not in a hurry.
 *
 * @par Format (in the blocks)
 * Block: header `magic 'nSt1' (4) seq (4) len (2) crc16 (2)`, then `len` bytes of
 * records, then 0xFF. Record: `id (2) type (1) ver (1) len (1) reserved (3)` and the data
 * padded to 4 bytes, so that every write is aligned for SPI flash. The header is written
 * last; a block whose header or CRC is wrong is not an image. All values little-endian.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "rtos.h"

#ifdef STORE_CONFIG_H
#include "store_config.h"
#endif

/* ---- configuration, overridable from the application's store_config.h --------------- */

/** Ceiling of the mutex that guards the RAM image: at least the priority of every task
    that calls store_*. The default, the highest priority, is always right. */
#ifndef STORE_CFG_MUTEX_CEILING
#define STORE_CFG_MUTEX_CEILING RTOS_CFG_NUM_TASKS
#endif

/** Optional: `bool f(uint16_t id, const void *pData, uint8_t len)` called by store_set()
    before a value is accepted; false makes store_set() return STORE_ERR_VALUE. The
    store checks id, type and length itself; what a valid hostname is, the application
    knows. */
/* #define STORE_CFG_VALIDATE_HOOK  myValidate */

/* ---- types ----------------------------------------------------------------------- */

/** @name Object types
 * @{ */
#define STORE_U8        1u      /**< 1 byte. */
#define STORE_U16       2u      /**< 2 bytes, little-endian in the block. */
#define STORE_U32       3u      /**< 4 bytes. */
#define STORE_I32       4u      /**< 4 bytes, two's complement. */
#define STORE_BOOL      5u      /**< 1 byte, 0 or 1. */
#define STORE_STR       6u      /**< Up to maxLen characters, no terminator stored. */
#define STORE_BLOB      7u      /**< Up to maxLen bytes, the application's layout. */
#define STORE_IP4       8u      /**< 4 bytes, network order (first octet first). */
/** @} */

/** @name Return values
 * @{ */
#define STORE_OK            0
#define STORE_ERR_ID        1   /**< No object with that id in the table. */
#define STORE_ERR_TYPE      2   /**< The call does not fit the object's type. */
#define STORE_ERR_LEN       3   /**< Longer than the object's room, or the table does not fit the buffer. */
#define STORE_ERR_VALUE     4   /**< The application's validation hook said no. */
#define STORE_ERR_FLASH     5   /**< The driver reported an error. */
#define STORE_ERR_VERIFY    6   /**< The image read back after a save does not match. */
#define STORE_ERR_FULL      7   /**< The records do not fit in one block. */
#define STORE_ERR_NOT_INIT  8   /**< store_init() has not been called, or failed. */
#define STORE_ERR_ARG       9   /**< NULL or an impossible argument. */
/** @} */

/** @name Object flags, in store_obj_t::flags
 * @{ */
#define STORE_F_DIRTY       0x01u   /**< Changed since the last save or revert. */
#define STORE_F_FROM_FLASH  0x02u   /**< The value came from the active block. */
/** @} */

/**
 * The flash driver, owned by the application. Addresses are relative to the start of
 * the store's area; block b starts at b * blockSize. read() may be called at any time
 * the store is used; write() and erase() only from store_save() and store_erase().
 * write() may rely on the bytes being 0xFF (erased) and on addr and n being multiples
 * of writeAlign. All three return false on a hardware error.
 */
typedef struct store_flash
{
    bool (*read)(uint32_t addr, void *pBuf, size_t n);
    bool (*write)(uint32_t addr, const void *pData, size_t n);
    bool (*erase)(uint8_t block);
    uint16_t blockSize;     /**< 4096 on the ESP32, half the EEPROM on the AVR. */
    uint8_t blocks;         /**< 2. */
    uint8_t writeAlign;     /**< 4 for SPI flash, 1 for EEPROM; divides blockSize. */
} store_flash_t;

/**
 * One object of the application's table. The application fills id, type, ver and
 * maxLen (`{ MY_ID, STORE_STR, 1, 32 }`); store_init() fills the rest.
 */
typedef struct store_obj
{
    uint16_t id;
    uint8_t type;           /**< STORE_U8 .. STORE_IP4. */
    uint8_t ver;            /**< The application's version of this object's meaning. */
    uint8_t maxLen;         /**< Room in the buffer; the fixed types need their size. */
    uint8_t len;            /**< Current length (set by the store). */
    uint8_t flags;          /**< STORE_F_* (set by the store). */
    uint8_t verInFlash;     /**< ver of the record the value came from (set by the store). */
    uint16_t offset;        /**< Into the buffer (set by the store). */
} store_obj_t;

/** A row of the table: `STORE_OBJ(MY_ID, STORE_STR, 32)`; version 1, the store's fields 0. */
#define STORE_OBJ(id, type, maxLen)     { (id), (type), 1, (maxLen), 0, 0, 0, 0 }

/** Size of a block header; the records start here. */
#define STORE_HEADER_SIZE   12u
/** Size of a record with @p len bytes of data. */
#define STORE_RECORD_SIZE(len)  (8u + (((uint16_t)(len) + 3u) & ~3u))

/* ---- setup -------------------------------------------------------------------------- */

/**
 * @brief Binds the store to its driver, table and buffer and loads the active image.
 *
 * Assigns every object its place in @p pBuf, reads both blocks, takes the valid one
 * with the highest sequence number as the image, and copies the records whose id and
 * type match an object of the table into the buffer (STORE_F_FROM_FLASH). Records of
 * unknown ids, of another type, or longer than the object's room are ignored and vanish
 * at the next save. Objects without a record keep len 0: the application sets its
 * default where STORE_F_FROM_FLASH is not set.
 *
 * @param pFlash  The driver; must live as long as the store (static).
 * @param pObjs   The table, writable (the store keeps its state in it).
 * @param count   Entries in the table, at least 1.
 * @param pBuf    The buffer for the values; the sum of all maxLen must fit.
 * @param bufSize Its size in bytes.
 * @return STORE_OK, or STORE_ERR_ARG, STORE_ERR_LEN (buffer too small, a fixed type with
 *         the wrong maxLen, writeAlign that does not divide blockSize), STORE_ERR_FLASH
 *         (a read failed). An empty store (no valid block) is STORE_OK with store_seq() 0.
 *
 * @par Context
 *     main(), after rtos_init() and before rtos_start(), once.
 */
int store_init(const store_flash_t *pFlash, store_obj_t *pObjs, uint8_t count,
               uint8_t *pBuf, size_t bufSize);

/**
 * @brief store_init() on nornCore's flash HAL (hal/flash.h): the common case in one call.
 *
 * store_halFlash is the driver (hal_flashRead/Write/EraseBlock and the HAL's block size,
 * count and alignment); store_initHal() runs hal_flashInit() and then store_init() with
 * it. On a board only (src/store_hal.c); a host test gives its own driver.
 *
 * @return As store_init(); STORE_ERR_FLASH if hal_flashInit() failed.
 */
extern const store_flash_t store_halFlash;
int store_initHal(store_obj_t *pObjs, uint8_t count, uint8_t *pBuf, size_t bufSize);

/* ---- defaults: set unless the value came from flash ---------------------------------- */

/** Sets the value if the object has none from flash (STORE_F_FROM_FLASH not set), so that
    a fresh device gets its defaults, saves them, and a configured one keeps what it has.
    Return as store_set(); STORE_OK when the stored value was kept. */
int store_default(uint16_t id, const void *pData, uint8_t len);
int store_defaultStr(uint16_t id, const char *s);
int store_defaultU32(uint16_t id, uint32_t value);

/* ---- the RAM image --------------------------------------------------------------- */

/** Copies the value of @p id into @p pOut (at most @p maxLen bytes) and its length into
    @p pLen (may be NULL). A STORE_STR is not terminated: use store_getStr(). */
int store_get(uint16_t id, void *pOut, uint8_t maxLen, uint8_t *pLen);

/** Sets the value of @p id; @p len must fit the object and, for the fixed types, equal
    their size. Marks the object dirty. The validation hook is asked first. */
int store_set(uint16_t id, const void *pData, uint8_t len);

/** STORE_STR as a C string into @p pOut of @p size bytes (terminated, truncated if
    needed; the whole string fits when size > maxLen). */
int store_getStr(uint16_t id, char *pOut, uint8_t size);
/** STORE_STR from a C string; its length must fit the object. */
int store_setStr(uint16_t id, const char *s);

/** STORE_U8, STORE_U16, STORE_U32, STORE_BOOL and STORE_IP4 as a number; STORE_I32 as
    well, reinterpreted. STORE_ERR_TYPE for the others. An object never set reads 0 and
    returns STORE_OK with *pSet false (pSet may be NULL). */
int store_getU32(uint16_t id, uint32_t *pOut, bool *pSet);
/** The number types; the value must fit the type (STORE_ERR_VALUE otherwise). */
int store_setU32(uint16_t id, uint32_t value);
int store_getI32(uint16_t id, int32_t *pOut, bool *pSet);
int store_setI32(uint16_t id, int32_t value);

/** True if any object changed since the last save or revert. */
bool store_isDirty(void);

/** Optional: \a fn is called with the object's id and \a arg after every successful
    store_set() (and so after store_setStr(), store_setU32(), store_setI32() and the
    `config set` of the console), in the caller's context, outside the store's lock.
    For values the application mirrors in RAM, a log level or an interval, so that a
    change from the console takes effect at once. NULL (the start value) turns it off.
    store_revert(), store_erase() and the load in store_init() do not call it: they change
    the whole image, and the application knows when it calls them. */
void store_onChange(void (*fn)(uint16_t id, void *arg), void *arg);

/** Drops every change: reloads the image from the active block (or clears all values
    when there is none). */
int store_revert(void);

/* ---- flash -------------------------------------------------------------------------- */

/**
 * @brief Writes the RAM image to the inactive block and makes it the active one.
 *
 * Erases the inactive block, writes the records, writes the header with the next
 * sequence number last, reads the block back and checks it. Returns STORE_OK and clears
 * the dirty flags; STORE_ERR_FULL (nothing written) if the records do not fit a block;
 * STORE_ERR_FLASH or STORE_ERR_VERIFY, in which case the previous image is still the
 * active one and the RAM image is unchanged, dirty flags included.
 *
 * @par Context
 *     Task (or main() before rtos_start()). Blocks the caller for the duration; see the
 *     file comment and hal/flash.h for how long and with what interrupts masked.
 */
int store_save(void);

/** Erases both blocks: factory state. The RAM image is cleared too. */
int store_erase(void);

/* ---- introspection, for a console and the application ----------------------------- */

uint8_t store_count(void);                          /**< Objects in the table. */
const store_obj_t *store_objAt(uint8_t index);      /**< NULL beyond the table. */
const store_obj_t *store_objById(uint16_t id);      /**< NULL if unknown. */
/** Pointer to the object's current value in the buffer (not terminated, @p len bytes);
    for a console that prints it. Read under the store's lock or before rtos_start(). */
const uint8_t *store_valuePtr(const store_obj_t *pObj);
uint32_t store_seq(void);                           /**< Sequence of the active block; 0 if none. */
int8_t store_activeBlock(void);                     /**< 0, 1, or -1 if none. */
/** Bytes a save would write (header and records) and the block size, for `config info`. */
uint16_t store_imageSize(void);
uint16_t store_blockSize(void);

/** Reads the saved value of @p id straight from the active block, bypassing the RAM
    image: what `config diff` compares against. *pLen 0 if the block has no such record. */
int store_getSaved(uint16_t id, void *pOut, uint8_t maxLen, uint8_t *pLen);

#endif  /* STORE_INCLUDED */
