# nornStore

Persistent configuration for nornRTOS applications: a small set of typed objects kept in
RAM and saved as a whole to two blocks of flash or EEPROM in turn, so that a power
failure during a save leaves the old or the new image, never a mixture. An optional
module on nornCore's public API: an application that does not call `store_init()` gets
no byte of it, and the kernel is not changed for it.

    include/store.h          the public API: store_flash_t, store_obj_t, init, get/set, save
    src/store.c              load at init, the RAM image, the A/B save, introspection
    src/store_crc.c          CRC-16/CCITT
    include/store_cmd.h      the `config` command for nosh: store_cmd_config, store_cmd_setNames
    src/store_cmd_config.c   its implementation; compiled only with nosh.mk in MODULES, linked
                             only when registered
    nornstore.mk             makefile fragment for an application's app.mk
    tests/                   host test against a flash simulated in RAM (make host-test)

## Using it

```c
#include "store.h"

enum { ID_HOSTNAME = 0x0101, ID_IP = 0x0201, ID_MODE = 0x0301 };

static store_obj_t objs[] =
{
    STORE_OBJ(ID_HOSTNAME, STORE_STR, 32),       /* id, type, room */
    STORE_OBJ(ID_IP,       STORE_IP4,  4),
    STORE_OBJ(ID_MODE,     STORE_U8,   1),
};
static uint8_t values[64];

int main(void)
{
    rtos_init();
    store_initHal(objs, 3, values, sizeof values);  /* nornCore's flash HAL as the driver */
    store_defaultStr(ID_HOSTNAME, "norn-01");       /* first boot: the default; a saved value is kept */
    store_defaultU32(ID_MODE, 1);
    ...
```

`store_initHal` is `hal_flashInit()` plus `store_init()` with `store_halFlash`, the driver
table over nornCore's `hal/flash.h`; an application with its own flash (or a host test)
fills a `store_flash_t` and calls `store_init()` itself:

```c
static const store_flash_t flash = { myRead, myWrite, myErase, 4096, 2, 4 };
    store_init(&flash, objs, 3, values, sizeof values);
    rtos_start();
}

/* later, in a task */
store_setU32(ID_MODE, 2);
if(store_isDirty())
    store_save();                                   /* tens of ms on an ESP32, see store.h */
```

and in app.mk: `MODULES += $(NORN_RTOS)/modules/nornStore/nornstore.mk`. With nosh in the
build as well, `nosh_add(&store_cmd_config)` and a name table (`store_cmd_setNames`) put
the objects on the console:

    nosh> config
    system.hostname   str  norn-001
    network.ip        ip4  -
    nosh> config set network.ip 10.0.0.5
    network.ip        ip4  10.0.0.5 *
    nosh> config diff
    network.ip        - -> 10.0.0.5
    nosh> config save
    saved, seq 2 in block 1

(`config get|set|diff|save|revert|erase yes|info`; objects by name or `0xID`.) A value the
application mirrors in RAM follows the console at once through `store_onChange(fn, arg)`:
`fn` is called with the object's id after every successful set, from the caller's task
and outside the store's lock (not by revert, erase or the load at init). `examples/device`
keeps nornCore's log level that way: `config set system.loglvl 2` quiets the console on
the spot, `config save` keeps it. The application owns the table, the buffer and the
driver; the store keeps a mutex and a few pointers.
Objects have a 16 bit id, a type (`STORE_U8 .. STORE_IP4`, `STORE_STR`, `STORE_BLOB`), a
version byte and up to 255 bytes. Names such as `system.hostname` live in the
application, as the task names do for nosh. Values never set read as empty with
`STORE_OK`; `store_getU32`'s `pSet` tells the two apart.

## Status

v0.1.0: phase 2B of nornInternal/nornStore/nornstore-implementationsplan.md; `store_onChange` added
2026-11-13. The API, the RAM image, the
block format and the A/B save, tested on the host against a simulated flash with a power
failure injected after every byte of a save (the old image survives, every time); the
ESP32 (SPI flash through the ROM) and AVR (EEPROM) drivers in nornCore's HAL
(hal/flash.h); the `config` command for nosh; `examples/store` in nornRTOS, whose
configuration survives reboots in QEMU and on an ESP32 board and can be changed from the
console; the size contract (tools/size_check.sh) on all three boards. Not yet seen: a
reboot on an AVR board (simavr does not keep the EEPROM). Not in v0.1: a log for state
written often, more than two blocks, writing with the cache on, objects over 255 bytes.
Measurements in nornInternal/nornStore/measurements.md; the user guide section is in nornDocs.

## Tests

    make host-test     tests/test_store.c on nornCore's host port, ASan and UBSan
    make analyze       gcc -fanalyzer over src/
    make size          object sizes for the UNO and the ESP32

## Licence

Business Source License 1.1 with the Additional Use Grant in LICENSE.md (Tenforward AB);
no third-party code, THIRD_PARTY_LICENSES.md.
