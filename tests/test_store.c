/**
 * @file test_store.c
 * nornStore on nornCore's host port against a flash simulated in RAM with the real
 * semantics of the medium: erase sets a block to 0xFF, a write may only clear bits (a
 * 0->1 without erase is an error the test counts), addresses and lengths must respect
 * writeAlign, and a write can be made to fail after N bytes (a power failure in the
 * middle of a save). The scripting task runs the scenarios; a second task reads the
 * store at the same time so that the lock is exercised.
 *
 * Step 2B.1 scenarios: init on an empty flash, the table checks, get/set of every type
 * with every error code, a save and a reload from flash, revert, a second save landing
 * in the other block with the next sequence number, the unknown record that is dropped,
 * the record of another type that is ignored, store_getSaved, erase.
 *
 * Step 2B.2 scenarios: a power failure after every single byte of a save, each followed
 * by a "reboot" (store_init on the flash as it was left) that must find either the whole
 * old image or the whole new one; the failed save leaves the RAM image and the dirty
 * flags alone and the next save succeeds; a corrupted active block falls back to the
 * other one; both corrupted is an empty store; one erase per save, also after failures.
 *
 * And the change hook: called after every successful set with the id, not after a
 * refused one nor by save, revert or erase; off again with NULL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtos.h"
#include "store.h"

#define T_READER    1u
#define T_SCRIPT    2u

static uint8_t _stackReader[1], _stackScript[1];
static unsigned _checks, _fails;
static uint32_t _idleLoops;
static volatile bool _done;

#define CHECK(cond)  check((cond), #cond, __LINE__)
static void check(bool ok, const char *what, int line)
{
    ++ _checks;
    if(!ok)
    {
        ++ _fails;
        printf("CHECK FAILED line %d: %s\n", line, what);
    }
}

/* ---- the simulated flash ------------------------------------------------------------ */

#define BLOCK_SIZE  256u            /* small, so that STORE_ERR_FULL is easy to reach */
#define BLOCKS      2u
#define ALIGN       4u

static uint8_t _flash[BLOCKS * BLOCK_SIZE];
static unsigned _erases, _writes, _badWrites, _misaligned;
static long _failAfter = -1;        /* bytes of write accepted before failing; -1 never */
static long _written;

static bool fRead(uint32_t addr, void *pBuf, size_t n)
{
    if(addr + n > sizeof _flash)
        return false;
    memcpy(pBuf, &_flash[addr], n);
    return true;
}

static bool fWrite(uint32_t addr, const void *pData, size_t n)
{
    size_t i;
    if(addr + n > sizeof _flash)
        return false;
    if(addr % ALIGN != 0  ||  n % ALIGN != 0)
        ++ _misaligned;
    ++ _writes;
    for(i = 0; i < n; ++i)
    {
        const uint8_t v = ((const uint8_t *)pData)[i];
        if(_failAfter >= 0  &&  _written >= _failAfter)
            return false;                           /* power gone: nothing more lands */
        if((v & ~_flash[addr + i]) != 0)
            ++ _badWrites;                          /* a 0 -> 1 without erase */
        _flash[addr + i] &= v;
        ++ _written;
    }
    return true;
}

static bool fErase(uint8_t block)
{
    if(block >= BLOCKS)
        return false;
    ++ _erases;
    memset(&_flash[(size_t)block * BLOCK_SIZE], 0xFF, BLOCK_SIZE);
    return true;
}

static const store_flash_t _driver = { fRead, fWrite, fErase, BLOCK_SIZE, BLOCKS, ALIGN };

/* ---- the application's table ------------------------------------------------------- */

#define ID_HOSTNAME     0x0101u
#define ID_MODE         0x0102u
#define ID_INTERVAL     0x0103u
#define ID_OFFSET       0x0104u
#define ID_ENABLED      0x0105u
#define ID_IP           0x0201u
#define ID_CAL          0x0301u
#define ID_FLAGS16      0x0302u

static store_obj_t _objs[] =
{
    { ID_HOSTNAME, STORE_STR,  1, 16, 0, 0, 0, 0 },
    { ID_MODE,     STORE_U8,   1,  1, 0, 0, 0, 0 },
    { ID_INTERVAL, STORE_U32,  1,  4, 0, 0, 0, 0 },
    { ID_OFFSET,   STORE_I32,  1,  4, 0, 0, 0, 0 },
    { ID_ENABLED,  STORE_BOOL, 1,  1, 0, 0, 0, 0 },
    { ID_IP,       STORE_IP4,  1,  4, 0, 0, 0, 0 },
    { ID_CAL,      STORE_BLOB, 2,  8, 0, 0, 0, 0 },
    { ID_FLAGS16,  STORE_U16,  1,  2, 0, 0, 0, 0 },
};
#define N_OBJS  (sizeof _objs / sizeof *_objs)
static uint8_t _buf[64];

static void resetTable(void)
{
    size_t i;
    for(i = 0; i < N_OBJS; ++i)
    {
        _objs[i].len = 0; _objs[i].flags = 0; _objs[i].verInFlash = 0; _objs[i].offset = 0;
    }
}

/* ---- the reader task: reads the store all the time ----------------------------------- */

static volatile unsigned _readerRounds;

static void reader(void *arg)
{
    (void)arg;
    for(;;)
    {
        char name[17];
        uint32_t v;
        (void)store_getStr(ID_HOSTNAME, name, sizeof name);
        (void)store_getU32(ID_INTERVAL, &v, NULL);
        ++ _readerRounds;
        rtos_delay(1);
    }
}

/* ---- the scenarios --------------------------------------------------------------- */

static void scenarioTableChecks(void)
{
    store_obj_t bad[2];
    uint8_t small[4];

    CHECK(store_init(NULL, _objs, N_OBJS, _buf, sizeof _buf) == STORE_ERR_ARG);
    CHECK(store_init(&_driver, _objs, 0, _buf, sizeof _buf) == STORE_ERR_ARG);
    CHECK(store_init(&_driver, _objs, N_OBJS, small, sizeof small) == STORE_ERR_LEN);
    CHECK(store_get(ID_MODE, small, 1, NULL) == STORE_ERR_NOT_INIT);      /* failed init: not usable */

    bad[0] = _objs[1]; bad[0].maxLen = 2;                                  /* U8 with room 2 */
    CHECK(store_init(&_driver, bad, 1, _buf, sizeof _buf) == STORE_ERR_LEN);
    bad[0] = _objs[1]; bad[0].type = 99;
    CHECK(store_init(&_driver, bad, 1, _buf, sizeof _buf) == STORE_ERR_TYPE);
    bad[0] = _objs[1]; bad[1] = _objs[1];                                  /* duplicate id */
    CHECK(store_init(&_driver, bad, 2, _buf, sizeof _buf) == STORE_ERR_ID);
    {
        store_flash_t d = _driver; d.writeAlign = 3;
        CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
        CHECK(store_init(&d, _objs, N_OBJS, _buf, sizeof _buf) == STORE_ERR_LEN);
    }
    resetTable();
}

static void scenarioEmptyAndTypes(void)
{
    char name[17] = "x";
    uint32_t u = 7; int32_t s = 7; bool set = true;
    uint8_t raw[8], len = 9;
    size_t i;

    memset(_flash, 0xFF, sizeof _flash);
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_seq() == 0  &&  store_activeBlock() == -1  &&  !store_isDirty());
    CHECK(store_count() == N_OBJS  &&  store_objAt(N_OBJS) == NULL  &&  store_objAt(0)->id == ID_HOSTNAME);
    for(i = 1; i < N_OBJS; ++i)                                            /* offsets are packed */
        CHECK(_objs[i].offset == _objs[i - 1].offset + _objs[i - 1].maxLen);

    /* never set: empty values, pSet false, OK */
    CHECK(store_getStr(ID_HOSTNAME, name, sizeof name) == STORE_OK  &&  name[0] == '\0');
    CHECK(store_getU32(ID_INTERVAL, &u, &set) == STORE_OK  &&  u == 0  &&  !set);
    CHECK(store_get(ID_CAL, raw, sizeof raw, &len) == STORE_OK  &&  len == 0);

    /* errors */
    CHECK(store_get(0x9999, raw, sizeof raw, NULL) == STORE_ERR_ID);
    CHECK(store_set(0x9999, raw, 1) == STORE_ERR_ID);
    CHECK(store_getU32(ID_HOSTNAME, &u, NULL) == STORE_ERR_TYPE);
    CHECK(store_setStr(ID_MODE, "a") == STORE_ERR_TYPE);
    CHECK(store_getStr(ID_MODE, name, sizeof name) == STORE_ERR_TYPE);
    CHECK(store_setI32(ID_INTERVAL, -1) == STORE_ERR_TYPE);
    CHECK(store_setStr(ID_HOSTNAME, "seventeen-chars--") == STORE_ERR_LEN);
    CHECK(store_set(ID_MODE, raw, 2) == STORE_ERR_LEN);                   /* fixed size */
    CHECK(store_set(ID_CAL, raw, 9) == STORE_ERR_LEN);
    CHECK(store_setU32(ID_MODE, 256) == STORE_ERR_VALUE);
    CHECK(store_setU32(ID_FLAGS16, 0x10000ul) == STORE_ERR_VALUE);
    CHECK(store_setU32(ID_ENABLED, 2) == STORE_ERR_VALUE);
    raw[0] = 2;
    CHECK(store_set(ID_ENABLED, raw, 1) == STORE_ERR_VALUE);
    CHECK(store_get(ID_MODE, NULL, 1, NULL) == STORE_ERR_ARG);
    CHECK(!store_isDirty());

    /* defaults on an empty store: nothing came from flash, so they are set */
    CHECK(store_defaultStr(ID_HOSTNAME, "dflt") == STORE_OK);
    CHECK(store_getStr(ID_HOSTNAME, name, sizeof name) == STORE_OK  &&  strcmp(name, "dflt") == 0);
    CHECK(store_defaultU32(ID_MODE, 7) == STORE_OK);
    CHECK(store_getU32(ID_MODE, &u, &set) == STORE_OK  &&  u == 7  &&  set);
    for(i = 0; i < 3; ++i) raw[i] = (uint8_t)i;
    CHECK(store_default(ID_CAL, raw, 3) == STORE_OK);
    CHECK(store_get(ID_CAL, raw, sizeof raw, &len) == STORE_OK  &&  len == 3);
    CHECK(store_defaultStr(0x9999, "x") == STORE_ERR_ID);

    /* every type once */
    CHECK(store_setStr(ID_HOSTNAME, "norn-01") == STORE_OK);
    CHECK(store_setU32(ID_MODE, 3) == STORE_OK);
    CHECK(store_setU32(ID_INTERVAL, 123456789ul) == STORE_OK);
    CHECK(store_setI32(ID_OFFSET, -42) == STORE_OK);
    CHECK(store_setU32(ID_ENABLED, 1) == STORE_OK);
    CHECK(store_setU32(ID_IP, 0xC0A80164ul) == STORE_OK);                 /* 192.168.1.100 */
    for(i = 0; i < 5; ++i) raw[i] = (uint8_t)(0xA0 + i);
    CHECK(store_set(ID_CAL, raw, 5) == STORE_OK);
    CHECK(store_setU32(ID_FLAGS16, 0xBEEF) == STORE_OK);
    CHECK(store_isDirty()  &&  (store_objById(ID_MODE)->flags & STORE_F_DIRTY));

    CHECK(store_getStr(ID_HOSTNAME, name, sizeof name) == STORE_OK  &&  strcmp(name, "norn-01") == 0);
    CHECK(store_getStr(ID_HOSTNAME, name, 4) == STORE_ERR_LEN  &&  strcmp(name, "nor") == 0);  /* truncated */
    CHECK(store_getU32(ID_MODE, &u, &set) == STORE_OK  &&  u == 3  &&  set);
    CHECK(store_getU32(ID_INTERVAL, &u, NULL) == STORE_OK  &&  u == 123456789ul);
    CHECK(store_getI32(ID_OFFSET, &s, NULL) == STORE_OK  &&  s == -42);
    CHECK(store_getU32(ID_ENABLED, &u, NULL) == STORE_OK  &&  u == 1);
    CHECK(store_getU32(ID_IP, &u, NULL) == STORE_OK  &&  u == 0xC0A80164ul);
    CHECK(store_get(ID_IP, raw, 4, &len) == STORE_OK  &&  len == 4  &&  raw[0] == 192  &&  raw[3] == 100);
    memset(raw, 0, sizeof raw);
    CHECK(store_get(ID_CAL, raw, sizeof raw, &len) == STORE_OK  &&  len == 5  &&  raw[0] == 0xA0  &&  raw[4] == 0xA4);
    CHECK(store_get(ID_CAL, raw, 2, &len) == STORE_ERR_LEN  &&  len == 5);  /* too small out */
    CHECK(store_getU32(ID_FLAGS16, &u, NULL) == STORE_OK  &&  u == 0xBEEF);

    /* revert on an empty store clears everything */
    CHECK(store_revert() == STORE_OK  &&  !store_isDirty());
    CHECK(store_getU32(ID_MODE, &u, &set) == STORE_OK  &&  u == 0  &&  !set);
}

static void fillValues(const char *host, uint32_t interval)
{
    uint8_t raw[5] = { 1, 2, 3, 4, 5 };
    CHECK(store_setStr(ID_HOSTNAME, host) == STORE_OK);
    CHECK(store_setU32(ID_MODE, 3) == STORE_OK);
    CHECK(store_setU32(ID_INTERVAL, interval) == STORE_OK);
    CHECK(store_setI32(ID_OFFSET, -42) == STORE_OK);
    CHECK(store_setU32(ID_ENABLED, 1) == STORE_OK);
    CHECK(store_setU32(ID_IP, 0xC0A80164ul) == STORE_OK);
    CHECK(store_set(ID_CAL, raw, 5) == STORE_OK);
    /* ID_FLAGS16 left unset on purpose: no record for it */
}

static void checkValues(const char *host, uint32_t interval)
{
    char name[17];
    uint32_t u; int32_t s; bool set;
    uint8_t raw[8], len;
    CHECK(store_getStr(ID_HOSTNAME, name, sizeof name) == STORE_OK  &&  strcmp(name, host) == 0);
    CHECK(store_getU32(ID_MODE, &u, NULL) == STORE_OK  &&  u == 3);
    CHECK(store_getU32(ID_INTERVAL, &u, NULL) == STORE_OK  &&  u == interval);
    CHECK(store_getI32(ID_OFFSET, &s, NULL) == STORE_OK  &&  s == -42);
    CHECK(store_getU32(ID_ENABLED, &u, NULL) == STORE_OK  &&  u == 1);
    CHECK(store_getU32(ID_IP, &u, NULL) == STORE_OK  &&  u == 0xC0A80164ul);
    CHECK(store_get(ID_CAL, raw, sizeof raw, &len) == STORE_OK  &&  len == 5  &&  raw[4] == 5);
    CHECK(store_getU32(ID_FLAGS16, &u, &set) == STORE_OK  &&  u == 0  &&  !set);
}

static void scenarioSaveAndReload(void)
{
    uint16_t expected;
    uint8_t raw[8], len;
    unsigned erasesBefore;

    memset(_flash, 0xFF, sizeof _flash);
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    fillValues("norn-01", 1000);
    /* 7 records: STR 7 -> 8+8, U8 -> 8+4, U32 -> 12, I32 -> 12, BOOL -> 12, IP4 -> 12, BLOB 5 -> 16 */
    expected = STORE_HEADER_SIZE + 16 + 12 + 12 + 12 + 12 + 12 + 16;
    CHECK(store_imageSize() == expected);
    CHECK(store_blockSize() == BLOCK_SIZE);

    erasesBefore = _erases; _misaligned = 0; _badWrites = 0;
    CHECK(store_save() == STORE_OK);
    CHECK(_erases == erasesBefore + 1);                                   /* exactly one erase */
    CHECK(_misaligned == 0  &&  _badWrites == 0);
    CHECK(store_seq() == 1  &&  store_activeBlock() == 0  &&  !store_isDirty());
    CHECK(store_objById(ID_MODE)->flags == STORE_F_FROM_FLASH);
    CHECK(store_objById(ID_FLAGS16)->flags == 0);                         /* unset: not from flash */
    CHECK(memcmp(_flash, "nSt1", 4) == 0);
    CHECK(_flash[BLOCK_SIZE] == 0xFF);                                     /* block 1 untouched */

    /* a fresh init (a reboot) sees the image */
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_seq() == 1  &&  store_activeBlock() == 0);
    checkValues("norn-01", 1000);
    CHECK(store_objById(ID_HOSTNAME)->flags == STORE_F_FROM_FLASH  &&  store_objById(ID_HOSTNAME)->verInFlash == 1);
    /* a default against a value from flash is a no-op; against an unset object it sets */
    CHECK(store_defaultStr(ID_HOSTNAME, "other") == STORE_OK  &&  !store_isDirty());
    checkValues("norn-01", 1000);
    CHECK(store_defaultU32(ID_FLAGS16, 0x55) == STORE_OK  &&  store_isDirty());
    { uint32_t v55 = 0; CHECK(store_getU32(ID_FLAGS16, &v55, NULL) == STORE_OK  &&  v55 == 0x55); }
    CHECK(store_revert() == STORE_OK);

    /* change, revert: back to the saved values */
    CHECK(store_setStr(ID_HOSTNAME, "changed") == STORE_OK  &&  store_isDirty());
    CHECK(store_revert() == STORE_OK  &&  !store_isDirty());
    checkValues("norn-01", 1000);

    /* change, save again: the other block, next sequence, old block still valid */
    CHECK(store_setStr(ID_HOSTNAME, "norn-02") == STORE_OK);
    CHECK(store_setU32(ID_INTERVAL, 2000) == STORE_OK);
    /* getSaved sees the flash, not the RAM change */
    CHECK(store_getSaved(ID_HOSTNAME, raw, sizeof raw, &len) == STORE_OK  &&  len == 7  &&  memcmp(raw, "norn-01", 7) == 0);
    CHECK(store_getSaved(ID_HOSTNAME, raw, 3, &len) == STORE_ERR_LEN  &&  len == 7);
    CHECK(store_getSaved(ID_FLAGS16, raw, sizeof raw, &len) == STORE_OK  &&  len == 0);
    CHECK(store_getSaved(0x9999, raw, sizeof raw, &len) == STORE_ERR_ID);
    CHECK(store_save() == STORE_OK);
    CHECK(store_seq() == 2  &&  store_activeBlock() == 1);
    CHECK(memcmp(_flash, "nSt1", 4) == 0  &&  memcmp(&_flash[BLOCK_SIZE], "nSt1", 4) == 0);
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_seq() == 2  &&  store_activeBlock() == 1);
    checkValues("norn-02", 2000);

    /* a third save goes back to block 0 */
    CHECK(store_setU32(ID_MODE, 3) == STORE_OK);                          /* same value, still a save */
    CHECK(store_save() == STORE_OK  &&  store_seq() == 3  &&  store_activeBlock() == 0);
}

static void scenarioForeignRecords(void)
{
    /* The image holds a record the table does not know (dropped at the next save), one
       with the right id but another type (ignored), and one too long (ignored). Built by
       saving with a different table. */
    static store_obj_t other[] =
    {
        { ID_HOSTNAME, STORE_STR,  1, 16, 0, 0, 0, 0 },
        { 0x0FFF,      STORE_U32,  1,  4, 0, 0, 0, 0 },     /* unknown to the main table */
        { ID_MODE,     STORE_U16,  1,  2, 0, 0, 0, 0 },     /* main table: U8 */
        { ID_CAL,      STORE_BLOB, 1, 12, 0, 0, 0, 0 },     /* main table: room 8 */
    };
    uint8_t twelve[12] = { 0 };
    uint32_t u; bool set;
    uint8_t raw[16], len;

    memset(_flash, 0xFF, sizeof _flash);
    CHECK(store_init(&_driver, other, 4, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_setStr(ID_HOSTNAME, "foreign") == STORE_OK);
    CHECK(store_setU32(0x0FFF, 5) == STORE_OK);
    CHECK(store_setU32(ID_MODE, 300) == STORE_OK);
    CHECK(store_set(ID_CAL, twelve, 12) == STORE_OK);
    CHECK(store_save() == STORE_OK  &&  store_seq() == 1);

    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_seq() == 1);
    CHECK(store_getStr(ID_HOSTNAME, (char *)raw, sizeof raw) == STORE_OK  &&  strcmp((char *)raw, "foreign") == 0);
    CHECK(store_getU32(ID_MODE, &u, &set) == STORE_OK  &&  !set);         /* other type: ignored */
    CHECK(store_get(ID_CAL, raw, sizeof raw, &len) == STORE_OK  &&  len == 0); /* too long: ignored */
    CHECK(store_getSaved(ID_MODE, raw, sizeof raw, &len) == STORE_OK  &&  len == 2); /* but it is in flash */

    CHECK(store_save() == STORE_OK  &&  store_seq() == 2);                /* rewritten: only ours */
    CHECK(store_imageSize() == STORE_HEADER_SIZE + 16);                   /* the hostname alone */
    resetTable();
    CHECK(store_init(&_driver, other, 4, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_getU32(0x0FFF, &u, &set) == STORE_OK  &&  !set);         /* the unknown one is gone */
}

static void scenarioFullAndErase(void)
{
    /* 256 byte blocks: 8 records fit (12 + 7*... ); make the blob and the string as big
       as their room and see the limit from the other side with a bigger table. */
    static store_obj_t big[] =
    {
        { 1, STORE_BLOB, 1, 200, 0, 0, 0, 0 },
        { 2, STORE_BLOB, 1,  40, 0, 0, 0, 0 },
    };
    static uint8_t bigBuf[240];
    uint8_t data[200];
    uint32_t u; bool set;

    memset(data, 0x5A, sizeof data);
    memset(_flash, 0xFF, sizeof _flash);
    CHECK(store_init(&_driver, big, 2, bigBuf, sizeof bigBuf) == STORE_OK);
    CHECK(store_set(1, data, 200) == STORE_OK);
    CHECK(store_set(2, data, 40) == STORE_OK);
    CHECK(store_imageSize() == STORE_HEADER_SIZE + 208 + 48);             /* 268 > 256 */
    {
        const unsigned erases = _erases;
        CHECK(store_save() == STORE_ERR_FULL);
        CHECK(_erases == erases  &&  store_seq() == 0  &&  store_isDirty()); /* nothing touched */
    }
    CHECK(store_set(2, data, 20) == STORE_OK);                             /* 12 + 208 + 32 = 252 */
    CHECK(store_save() == STORE_OK  &&  store_seq() == 1);

    /* erase: both blocks 0xFF, RAM cleared, next save is seq 1 in block 0 */
    CHECK(store_erase() == STORE_OK);
    CHECK(store_seq() == 0  &&  store_activeBlock() == -1);
    CHECK(_flash[0] == 0xFF  &&  _flash[BLOCK_SIZE] == 0xFF);
    CHECK(store_get(1, data, sizeof data, NULL) == STORE_OK  &&  store_objById(1)->len == 0);
    CHECK(store_set(1, data, 4) == STORE_OK  &&  store_save() == STORE_OK);
    CHECK(store_seq() == 1  &&  store_activeBlock() == 0);

    /* back to the main table for the rest */
    resetTable();
    memset(_flash, 0xFF, sizeof _flash);
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_getU32(ID_MODE, &u, &set) == STORE_OK  &&  !set);
}

static void scenarioPowerFailure(void)
{
    long total, n;
    unsigned mixed = 0, oldWins = 0, newWins = 0;

    /* Image A: seq 1 in block 0. Then measure how many bytes a save of image B writes. */
    memset(_flash, 0xFF, sizeof _flash);
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    fillValues("image-a", 1000);
    CHECK(store_save() == STORE_OK  &&  store_seq() == 1);
    fillValues("image-b", 2000);
    _written = 0;
    CHECK(store_save() == STORE_OK  &&  store_seq() == 2  &&  store_activeBlock() == 1);
    total = _written;
    CHECK(total == (long)(store_imageSize()));                            /* every byte once */

    for(n = 0; n < total; ++n)
    {
        uint8_t flashA[sizeof _flash];
        char host[17];
        uint32_t interval;

        /* back to: block 0 = image A (seq 1), block 1 = whatever, RAM = A */
        memset(_flash, 0xFF, sizeof _flash);
        resetTable();
        CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
        fillValues("image-a", 1000);
        CHECK(store_save() == STORE_OK  &&  store_seq() == 1  &&  store_activeBlock() == 0);
        memcpy(flashA, _flash, sizeof _flash);

        /* change to B, lose power after n bytes of the save */
        fillValues("image-b", 2000);
        _written = 0;
        _failAfter = n;
        CHECK(store_save() == STORE_ERR_FLASH);
        _failAfter = -1;
        /* the RAM image is B and dirty, block 0 untouched, the store still on A */
        CHECK(store_isDirty()  &&  store_seq() == 1  &&  store_activeBlock() == 0);
        CHECK(memcmp(flashA, _flash, BLOCK_SIZE) == 0);
        checkValues("image-b", 2000);

        /* reboot on the flash as it was left: A, whole */
        resetTable();
        CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
        CHECK(store_getStr(ID_HOSTNAME, host, sizeof host) == STORE_OK);
        CHECK(store_getU32(ID_INTERVAL, &interval, NULL) == STORE_OK);
        if(strcmp(host, "image-a") == 0  &&  interval == 1000  &&  store_seq() == 1)
            ++ oldWins;
        else if(strcmp(host, "image-b") == 0  &&  interval == 2000  &&  store_seq() == 2)
            ++ newWins;
        else
            ++ mixed;

        /* and the save works at the next attempt, with one more erase */
        {
            const unsigned erases = _erases;
            fillValues("image-b", 2000);
            CHECK(store_save() == STORE_OK  &&  store_seq() == 2  &&  store_activeBlock() == 1);
            CHECK(_erases == erases + 1);
            resetTable();
            CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
            checkValues("image-b", 2000);
        }
    }
    printf("power failure at every byte of %ld: old image %u times, new %u, mixed %u\n"
           , total, oldWins, newWins, mixed);
    CHECK(mixed == 0);
    CHECK(oldWins == (unsigned)total);     /* the header is last and whole: the new image never
                                              wins before the save returned success */
}

static void scenarioCorruption(void)
{
    char host[17];

    /* A in block 0 (seq 1), B in block 1 (seq 2). Damage B: A is the image again. */
    memset(_flash, 0xFF, sizeof _flash);
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    fillValues("image-a", 1000);
    CHECK(store_save() == STORE_OK);
    fillValues("image-b", 2000);
    CHECK(store_save() == STORE_OK  &&  store_activeBlock() == 1);

    _flash[BLOCK_SIZE + STORE_HEADER_SIZE + 10] ^= 0x01;                 /* a data bit in B */
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_seq() == 1  &&  store_activeBlock() == 0);
    CHECK(store_getStr(ID_HOSTNAME, host, sizeof host) == STORE_OK  &&  strcmp(host, "image-a") == 0);

    /* a save now goes to block 1 (the damaged one is simply rewritten) with seq 2 */
    fillValues("image-c", 3000);
    CHECK(store_save() == STORE_OK  &&  store_seq() == 2  &&  store_activeBlock() == 1);

    /* damage the magic of block 1 and the length of block 0: empty store */
    _flash[BLOCK_SIZE] = 0;
    _flash[8] = 0xFF; _flash[9] = 0xFF;                                   /* len 0xFFFF > block */
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_seq() == 0  &&  store_activeBlock() == -1);
    CHECK(store_getStr(ID_HOSTNAME, host, sizeof host) == STORE_OK  &&  host[0] == '\0');

    /* a valid header whose records are truncated (len says more than the records hold):
       the walk stops at the truncation and keeps what it had */
    fillValues("image-d", 4000);
    CHECK(store_save() == STORE_OK  &&  store_activeBlock() == 0);
    _flash[STORE_HEADER_SIZE + 4] = 200;                                  /* first record claims 200 B */
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_seq() == 0);                                              /* CRC catches it first */
}

/* ---- the change hook ---------------------------------------------------------------- */

static uint16_t _changedId;
static unsigned _changes;
static void *_changeArg;

static void onChange(uint16_t id, void *arg)
{
    _changedId = id;
    _changeArg = arg;
    ++ _changes;
}

static void scenarioChangeHook(void)
{
    int marker = 0;
    uint8_t raw[1] = { 1 };

    memset(_flash, 0xFF, sizeof _flash);
    resetTable();
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);

    /* off at start: no call */
    CHECK(store_setU32(ID_MODE, 1) == STORE_OK  &&  _changes == 0);

    store_onChange(onChange, &marker);
    CHECK(store_setU32(ID_MODE, 2) == STORE_OK);
    CHECK(_changes == 1  &&  _changedId == ID_MODE  &&  _changeArg == &marker);
    CHECK(store_setStr(ID_HOSTNAME, "a") == STORE_OK  &&  _changes == 2  &&  _changedId == ID_HOSTNAME);
    CHECK(store_set(ID_ENABLED, raw, 1) == STORE_OK  &&  _changes == 3  &&  _changedId == ID_ENABLED);
    CHECK(store_setI32(ID_OFFSET, -5) == STORE_OK  &&  _changes == 4  &&  _changedId == ID_OFFSET);

    /* a refused set does not call it */
    CHECK(store_setU32(ID_MODE, 300) == STORE_ERR_VALUE  &&  _changes == 4);
    CHECK(store_set(0x9999, raw, 1) == STORE_ERR_ID  &&  _changes == 4);
    CHECK(store_setStr(ID_MODE, "x") == STORE_ERR_TYPE  &&  _changes == 4);

    /* save, revert and erase change nothing through the hook */
    CHECK(store_save() == STORE_OK  &&  _changes == 4);
    CHECK(store_setU32(ID_MODE, 3) == STORE_OK  &&  _changes == 5);
    CHECK(store_revert() == STORE_OK  &&  _changes == 5);
    CHECK(store_erase() == STORE_OK  &&  _changes == 5);

    /* off again */
    store_onChange(NULL, NULL);
    CHECK(store_setU32(ID_MODE, 4) == STORE_OK  &&  _changes == 5);
}

static void script(void *arg)
{
    (void)arg;
    /* A pause between the scenarios lets the reader task run against the store and the
       ticks advance; the store is used from two tasks from here on. */
    scenarioEmptyAndTypes();
    rtos_delay(5);
    scenarioSaveAndReload();
    rtos_delay(5);
    scenarioForeignRecords();
    rtos_delay(5);
    scenarioFullAndErase();
    rtos_delay(5);
    scenarioPowerFailure();
    rtos_delay(5);
    scenarioCorruption();
    rtos_delay(5);
    scenarioChangeHook();
    rtos_delay(5);
    CHECK(_readerRounds > 10);                                             /* the reader ran meanwhile */
    CHECK(_badWrites == 0  &&  _misaligned == 0);
    _done = true;
    rtos_delay(RTOS_FOREVER);
}

/* ---- the kernel's hooks ------------------------------------------------------------- */

void test_idleHook(void)
{
    if(_done)
    {
        printf("%s: test_store (%u checks, %u failures, %lu idle loops, %u erases %u writes)\n"
               , _fails == 0 ? "PASS" : "FAIL", _checks, _fails, (unsigned long)_idleLoops, _erases, _writes);
        exit(_fails == 0 ? 0 : 1);
    }
    if(++_idleLoops > 100000ul)
    {
        printf("FAIL: runaway, test did not finish (%u checks, %u failures)\n", _checks, _fails);
        exit(1);
    }
    rtos_host_tick();
}

void rtos_assertFailed(const char *file, uint16_t line)
{
    printf("ASSERTION FAILED: %s line %u (current task %u)\n", file, line, rtos_getCurrentTask());
    exit(2);
}

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    printf("PANIC reason %u task %u arg1 %lu arg2 %lu\n", pInfo->reason, pInfo->task
           , (unsigned long)pInfo->arg1, (unsigned long)pInfo->arg2);
    exit(3);
}

int main(void)
{
    rtos_init();
    /* Before rtos_start(): the table checks and a first init from main(), as an
       application does when it needs its configuration to create its tasks. */
    scenarioTableChecks();
    memset(_flash, 0xFF, sizeof _flash);
    CHECK(store_init(&_driver, _objs, N_OBJS, _buf, sizeof _buf) == STORE_OK);
    CHECK(store_setStr(ID_HOSTNAME, "from-main") == STORE_OK);           /* no lock needed here */
    CHECK(store_save() == STORE_OK  &&  store_seq() == 1);

    rtos_createTask(T_READER, reader, NULL, _stackReader, sizeof _stackReader);
    rtos_createTask(T_SCRIPT, script, NULL, _stackScript, sizeof _stackScript);
    rtos_start();
    return 0;
}
