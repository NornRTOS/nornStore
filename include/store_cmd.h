#ifndef STORE_CMD_INCLUDED
#define STORE_CMD_INCLUDED
/**
 * @file store_cmd.h
 * @brief The `config` command of nornStore for the nosh shell (nornSHELL).
 *
 * Registered by the application with `nosh_add(&store_cmd_config)`; linked in only then.
 * nornstore.mk compiles src/store_cmd_config.c only when nosh.mk is among the
 * application's MODULES, so an application without the shell never sees this file.
 *
 *     config                    every object: name, type, value, `*` if changed since the save
 *     config get <obj>          one value
 *     config set <obj> <value>  a value; a string takes the rest of the line
 *     config diff               the changed objects: saved value -> current value
 *     config save               store_save()
 *     config revert             store_revert()
 *     config erase yes          store_erase(): factory state, both blocks
 *     config info               active block, sequence, image size, block size, dirty
 *
 * `<obj>` is a name from the table given to store_cmd_setNames(), or the id in hex
 * (`0x0101`). Values: numbers in decimal, booleans as 0/1/true/false/on/off, IPv4 as
 * a.b.c.d, blobs as hex digits (two per byte), strings as typed (runs of spaces become
 * one).
 */

#include "nosh.h"
#include "store.h"

/** A name for an object id, for the console. */
typedef struct store_name
{
    uint16_t id;
    const char *name;           /**< RAM string, e.g. "system.hostname"; const data. */
} store_name_t;

/** Gives the command the names of the objects; without a table it shows ids in hex.
    The table is the application's const data and must live as long as the shell. */
void store_cmd_setNames(const store_name_t *pNames, uint8_t count);

extern nosh_cmd_t store_cmd_config;

#endif  /* STORE_CMD_INCLUDED */
