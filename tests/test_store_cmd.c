/**
 * @file test_store_cmd.c
 * The `config` command through nosh on the host: a scripted transport types the command
 * lines, the store sits on a flash simulated in RAM, and the output is checked line by
 * line. Covers the listing, get/set of every type with good and bad values, names and
 * hex ids, the string that takes the rest of the line, diff against the saved image,
 * save, revert, erase with and without the confirmation, info, and the usage line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtos.h"
#include "nosh.h"
#include "store.h"
#include "store_cmd.h"

#define T_SHELL     1u

static uint8_t _stackShell[1];
static unsigned _checks, _fails;
static uint32_t _idleLoops, _idleSinceSpent;

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

/* ---- the simulated flash (as in test_store.c, without the fault injection) ---------- */

#define BLOCK_SIZE  512u
static uint8_t _flash[2 * BLOCK_SIZE];

static bool fRead(uint32_t addr, void *pBuf, size_t n)
{
    if(addr + n > sizeof _flash) return false;
    memcpy(pBuf, &_flash[addr], n);
    return true;
}
static bool fWrite(uint32_t addr, const void *pData, size_t n)
{
    size_t i;
    if(addr + n > sizeof _flash) return false;
    for(i = 0; i < n; ++i)
        _flash[addr + i] &= ((const uint8_t *)pData)[i];
    return true;
}
static bool fErase(uint8_t block)
{
    if(block >= 2) return false;
    memset(&_flash[(size_t)block * BLOCK_SIZE], 0xFF, BLOCK_SIZE);
    return true;
}
static const store_flash_t _driver = { fRead, fWrite, fErase, BLOCK_SIZE, 2, 4 };

/* ---- the application's objects and names ------------------------------------------- */

enum { ID_HOST = 0x0101, ID_MODE = 0x0102, ID_PERIOD = 0x0103, ID_OFFSET = 0x0104,
       ID_ON = 0x0105, ID_IP = 0x0201, ID_CAL = 0x0301, ID_NONAME = 0x0F0F };

static store_obj_t _objs[] =
{
    { ID_HOST,   STORE_STR,  1, 16, 0, 0, 0, 0 },
    { ID_MODE,   STORE_U8,   1,  1, 0, 0, 0, 0 },
    { ID_PERIOD, STORE_U32,  1,  4, 0, 0, 0, 0 },
    { ID_OFFSET, STORE_I32,  1,  4, 0, 0, 0, 0 },
    { ID_ON,     STORE_BOOL, 1,  1, 0, 0, 0, 0 },
    { ID_IP,     STORE_IP4,  1,  4, 0, 0, 0, 0 },
    { ID_CAL,    STORE_BLOB, 1,  4, 0, 0, 0, 0 },
    { ID_NONAME, STORE_U16,  1,  2, 0, 0, 0, 0 },
};
static uint8_t _buf[48];
static const store_name_t _names[] =
{
    { ID_HOST, "system.hostname" }, { ID_MODE, "app.mode" }, { ID_PERIOD, "app.period" },
    { ID_OFFSET, "app.offset" }, { ID_ON, "app.enabled" }, { ID_IP, "network.ip" },
    { ID_CAL, "app.cal" },
};

/* ---- the scripted transport ------------------------------------------------------- */

static const char _script[] =
    "config\r"                                  /* empty store: all '-' */
    "config set system.hostname my little device\r"
    "config set app.mode 7\r"
    "config set app.mode 300\r"                 /* too big for u8 */
    "config set app.period 86400\r"
    "config set app.offset -15\r"
    "config set app.offset x\r"
    "config set app.enabled on\r"
    "config set app.enabled maybe\r"
    "config set network.ip 10.0.0.254\r"
    "config set network.ip 10.0.0.256\r"
    "config set app.cal deadbeef\r"
    "config set app.cal abc\r"                  /* odd digits */
    "config set 0x0F0F 65535\r"                 /* by hex id, no name */
    "config set 0x0F0F 65536\r"
    "config set nosuch 1\r"
    "config get app.mode\r"
    "config get 0x0102\r"
    "config diff\r"                             /* everything new: - -> value */
    "config info\r"
    "config save\r"
    "config diff\r"                             /* nothing */
    "config set app.mode 8\r"
    "config set system.hostname other\r"
    "config diff\r"                             /* 7 -> 8, my little device -> other */
    "config revert\r"
    "config get app.mode\r"
    "config\r"
    "config erase\r"                            /* needs yes */
    "config get app.mode\r"
    "config erase yes\r"
    "config info\r"
    "config bogus\r";
static size_t _scriptPos = 0;
static bool _spent = false;
static char _out[8192];
static size_t _outLen = 0;

static size_t ioRead(void *buf, size_t n)
{
    static unsigned calls = 0;
    (void)n;
    if(_scriptPos >= sizeof(_script) - 1u) { _spent = true; return 0; }
    if(++calls % 8u == 0) return 0;
    *(char *)buf = _script[_scriptPos++];
    return 1;
}
static void ioWrite(const void *buf, size_t n)
{
    if(_outLen + n < sizeof _out)
    {
        memcpy(&_out[_outLen], buf, n);
        _outLen += n;
        _out[_outLen] = '\0';
    }
}
static const nosh_io_t _io = { ioRead, ioWrite };

/* ---- checks ------------------------------------------------------------------------- */

static unsigned countOf(const char *needle)
{
    unsigned n = 0;
    const char *p = _out;
    while((p = strstr(p, needle)) != NULL) { ++ n; p += strlen(needle); }
    return n;
}

static void checkOutput(void)
{
    /* the first listing: every object with a dash */
    CHECK(strstr(_out, "system.hostname   str  -\r\napp.mode          u8   -\r\n") != NULL);
    CHECK(strstr(_out, "0x0f0f            u16  -\r\n") != NULL);

    /* sets echo the new line with the dirty mark */
    CHECK(strstr(_out, "system.hostname   str  my little device *\r\n") != NULL);
    CHECK(strstr(_out, "app.mode          u8   7 *\r\n") != NULL);
    CHECK(strstr(_out, "app.period        u32  86400 *\r\n") != NULL);
    CHECK(strstr(_out, "app.offset        i32  -15 *\r\n") != NULL);
    CHECK(strstr(_out, "app.enabled       bool true *\r\n") != NULL);
    CHECK(strstr(_out, "network.ip        ip4  10.0.0.254 *\r\n") != NULL);
    CHECK(strstr(_out, "app.cal           blob deadbeef *\r\n") != NULL);
    CHECK(strstr(_out, "0x0f0f            u16  65535 *\r\n") != NULL);

    /* the bad values, one message each */
    CHECK(countOf("config: bad value for type u8   \r\n") == 1);
    CHECK(countOf("config: bad value for type i32  \r\n") == 1);
    CHECK(countOf("config: bad value for type bool \r\n") == 1);
    CHECK(countOf("config: bad value for type ip4  \r\n") == 1);
    CHECK(countOf("config: bad value for type blob \r\n") == 1);
    CHECK(countOf("config: bad value for type u16  \r\n") == 1);
    CHECK(strstr(_out, "config: unknown object nosuch\r\n") != NULL);

    /* get by name and by id give the same line */
    CHECK(countOf("app.mode          u8   7 *\r\n") >= 3);

    /* diff before the save: from '-' */
    CHECK(strstr(_out, "app.mode          - -> 7\r\n") != NULL);
    CHECK(strstr(_out, "system.hostname   - -> my little device\r\n") != NULL);
    CHECK(strstr(_out, "block -1 seq 0, image ") != NULL);
    CHECK(strstr(_out, " bytes, 8 objects, unsaved changes\r\n") != NULL);
    CHECK(strstr(_out, "saved, seq 1 in block 0\r\n") != NULL);
    CHECK(strstr(_out, "no changes\r\n") != NULL);

    /* diff after two changes: saved -> current */
    CHECK(strstr(_out, "system.hostname   my little device -> other\r\n") != NULL);
    CHECK(strstr(_out, "app.mode          7 -> 8\r\n") != NULL);
    CHECK(strstr(_out, "reverted\r\n") != NULL);
    CHECK(strstr(_out, "app.mode          u8   7\r\n") != NULL);            /* clean after revert */
    CHECK(strstr(_out, "system.hostname   str  my little device\r\n") != NULL);

    /* erase: refused without yes, then done */
    CHECK(strstr(_out, "config erase yes: erases both blocks and every value\r\n") != NULL);
    CHECK(strstr(_out, "erased\r\n") != NULL);
    CHECK(strstr(_out, "block -1 seq 0, image 12 of 512 bytes, 8 objects, saved\r\n") != NULL);
    CHECK(strstr(_out, "usage: config [") != NULL);
    CHECK(_flash[0] == 0xFF  &&  _flash[BLOCK_SIZE] == 0xFF);
}

/* ---- hooks and main ------------------------------------------------------------------ */

void test_idleHook(void)
{
    if(_spent  &&  ++_idleSinceSpent > 50)
    {
        checkOutput();
        if(_fails != 0  ||  getenv("NOSH_TEST_SHOW") != NULL)
            printf("---- shell output ----\n%s\n----------------------\n", _out);
        printf("%s: test_store_cmd (%u checks, %u failures, %lu idle loops)\n"
               , _fails == 0 ? "PASS" : "FAIL", _checks, _fails, (unsigned long)_idleLoops);
        exit(_fails == 0 ? 0 : 1);
    }
    if(++_idleLoops > 100000ul)
    {
        printf("FAIL: runaway (%u checks, %u failures)\n", _checks, _fails);
        exit(1);
    }
    rtos_host_tick();
}

void rtos_assertFailed(const char *file, uint16_t line)
{
    printf("ASSERTION FAILED: %s line %u\n", file, line);
    exit(2);
}

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    printf("PANIC reason %u task %u\n", pInfo->reason, pInfo->task);
    exit(3);
}

int main(void)
{
    rtos_init();
    memset(_flash, 0xFF, sizeof _flash);
    CHECK(store_init(&_driver, _objs, sizeof _objs / sizeof *_objs, _buf, sizeof _buf) == STORE_OK);
    CHECK(nosh_enable(&_io, T_SHELL, _stackShell, sizeof _stackShell) == NOSH_OK);
    nosh_add(&store_cmd_config);
    store_cmd_setNames(_names, sizeof _names / sizeof *_names);
    rtos_start();
    return 0;
}
