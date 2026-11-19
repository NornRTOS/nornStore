/**
 * @file store_cmd_config.c
 * `config` for nosh: the store's objects on the console. Values are formatted by type;
 * `set` parses them the same way. Everything here is printing and parsing; the store's
 * API does the work and its checks give the error messages.
 */

#include "store_cmd.h"

/** Longest value `diff` can compare, and `set` can build; bigger objects show "(long)". */
#ifndef STORE_CFG_CMD_BUF
#define STORE_CFG_CMD_BUF   64u
#endif

static const store_name_t *_pNames = NULL;
static uint8_t _nameCount = 0;
static uint8_t _scratch[STORE_CFG_CMD_BUF];


void store_cmd_setNames(const store_name_t *pNames, uint8_t count)
{
    _pNames = pNames;
    _nameCount = count;
}

/* ---- names and types -------------------------------------------------------------- */

static const char *nameOf(uint16_t id)
{
    uint8_t i;
    for(i = 0; i < _nameCount; ++i)
        if(_pNames[i].id == id)
            return _pNames[i].name;
    return NULL;
}


static void putName(uint16_t id, uint8_t width)
{
    const char *name = nameOf(id);
    if(name != NULL)
        nosh_putPad(name, width);
    else
    {
        nosh_putsP(NOSH_STR("0x"));
        nosh_putHex8((uint8_t)(id >> 8));
        nosh_putHex8((uint8_t)id);
        while(width-- > 6u)
            nosh_putc(' ');
    }
}


static void putType(uint8_t type)
{
    switch(type)
    {
    case STORE_U8:   nosh_putPadP(NOSH_STR("u8"), 5);   break;
    case STORE_U16:  nosh_putPadP(NOSH_STR("u16"), 5);  break;
    case STORE_U32:  nosh_putPadP(NOSH_STR("u32"), 5);  break;
    case STORE_I32:  nosh_putPadP(NOSH_STR("i32"), 5);  break;
    case STORE_BOOL: nosh_putPadP(NOSH_STR("bool"), 5); break;
    case STORE_STR:  nosh_putPadP(NOSH_STR("str"), 5);  break;
    case STORE_BLOB: nosh_putPadP(NOSH_STR("blob"), 5); break;
    case STORE_IP4:  nosh_putPadP(NOSH_STR("ip4"), 5);  break;
    default:         nosh_putPadP(NOSH_STR("?"), 5);    break;
    }
}


/** Resolves argv[i] to an object: a name, or 0xHHHH. NULL and a message if neither. */
static const store_obj_t *findObj(const char *arg)
{
    const store_obj_t *pO = NULL;
    uint8_t i;
    for(i = 0; i < _nameCount; ++i)
        if(nosh_strEq(arg, _pNames[i].name))
        {
            pO = store_objById(_pNames[i].id);
            break;
        }
    if(pO == NULL  &&  arg[0] == '0'  &&  (arg[1] == 'x'  ||  arg[1] == 'X'))
    {
        uint32_t id = 0;
        const char *p = arg + 2;
        bool ok = *p != '\0';
        while(*p != '\0'  &&  ok)
        {
            const char c = *p++;
            uint8_t d;
            if(c >= '0'  &&  c <= '9') d = (uint8_t)(c - '0');
            else if(c >= 'a'  &&  c <= 'f') d = (uint8_t)(c - 'a' + 10);
            else if(c >= 'A'  &&  c <= 'F') d = (uint8_t)(c - 'A' + 10);
            else { ok = false; break; }
            id = (id << 4) | d;
            if(id > 0xFFFFu)
                ok = false;
        }
        if(ok)
            pO = store_objById((uint16_t)id);
    }
    if(pO == NULL)
    {
        nosh_putsP(NOSH_STR("config: unknown object "));
        nosh_puts(arg);
        nosh_putEol();
    }
    return pO;
}

/* ---- values ------------------------------------------------------------------------- */

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}


/** Prints len bytes of a value of the given type. */
static void putValue(uint8_t type, const uint8_t *p, uint8_t len)
{
    uint8_t i;
    if(len == 0)
    {
        nosh_putc('-');
        return;
    }
    switch(type)
    {
    case STORE_U8:   nosh_putU32(p[0]); break;
    case STORE_U16:  nosh_putU32((uint32_t)p[0] | ((uint32_t)p[1] << 8)); break;
    case STORE_U32:  nosh_putU32(rd32le(p)); break;
    case STORE_I32:  nosh_putI32((int32_t)rd32le(p)); break;
    case STORE_BOOL: nosh_putsP(p[0] ? NOSH_STR("true") : NOSH_STR("false")); break;
    case STORE_IP4:
        for(i = 0; i < 4; ++i)
        {
            if(i > 0) nosh_putc('.');
            nosh_putU32(p[i]);
        }
        break;
    case STORE_STR:
        for(i = 0; i < len; ++i)
            nosh_putc((char)p[i]);
        break;
    default:                                    /* blob: hex */
        for(i = 0; i < len; ++i)
            nosh_putHex8(p[i]);
        break;
    }
}


static void putErr(const char *pWhatP, int res)
{
    nosh_putsP(NOSH_STR("config: "));
    nosh_putsP(pWhatP);
    switch(res)
    {
    case STORE_ERR_ID:      nosh_putsP(NOSH_STR(": unknown object")); break;
    case STORE_ERR_TYPE:    nosh_putsP(NOSH_STR(": wrong type")); break;
    case STORE_ERR_LEN:     nosh_putsP(NOSH_STR(": too long")); break;
    case STORE_ERR_VALUE:   nosh_putsP(NOSH_STR(": value refused")); break;
    case STORE_ERR_FLASH:   nosh_putsP(NOSH_STR(": flash error")); break;
    case STORE_ERR_VERIFY:  nosh_putsP(NOSH_STR(": verify failed, old image kept")); break;
    case STORE_ERR_FULL:    nosh_putsP(NOSH_STR(": image does not fit a block")); break;
    case STORE_ERR_NOT_INIT: nosh_putsP(NOSH_STR(": store not initialized")); break;
    default:                nosh_putsP(NOSH_STR(": error ")); nosh_putU32((uint32_t)res); break;
    }
    nosh_putEol();
}


/** Parses argv[from..argc-1] as a value of pO's type into _scratch; returns its length
    or 0 with a message. */
static uint8_t parseValue(const store_obj_t *pO, uint8_t argc, const char *const argv[], uint8_t from)
{
    const char *s = argv[from];
    uint32_t v;
    uint8_t n = 0;

    switch(pO->type)
    {
    case STORE_U8: case STORE_U16: case STORE_U32:
        if(!nosh_parseU32(s, &v))
            break;
        _scratch[0] = (uint8_t)v; _scratch[1] = (uint8_t)(v >> 8);
        _scratch[2] = (uint8_t)(v >> 16); _scratch[3] = (uint8_t)(v >> 24);
        n = pO->maxLen;
        if((pO->type == STORE_U8  &&  v > 0xFFu)  ||  (pO->type == STORE_U16  &&  v > 0xFFFFu))
            n = 0;
        break;
    case STORE_I32:
    {
        const bool neg = s[0] == '-';
        if(!nosh_parseU32(neg ? s + 1 : s, &v)  ||  v > (neg ? 0x80000000ul : 0x7FFFFFFFul))
            break;
        if(neg)
            v = 0u - v;
        _scratch[0] = (uint8_t)v; _scratch[1] = (uint8_t)(v >> 8);
        _scratch[2] = (uint8_t)(v >> 16); _scratch[3] = (uint8_t)(v >> 24);
        n = 4;
        break;
    }
    case STORE_BOOL:
        if(nosh_strEqP(s, NOSH_STR("1"))  ||  nosh_strEqP(s, NOSH_STR("true"))  ||  nosh_strEqP(s, NOSH_STR("on")))
            { _scratch[0] = 1; n = 1; }
        else if(nosh_strEqP(s, NOSH_STR("0"))  ||  nosh_strEqP(s, NOSH_STR("false"))  ||  nosh_strEqP(s, NOSH_STR("off")))
            { _scratch[0] = 0; n = 1; }
        break;
    case STORE_IP4:
    {
        uint8_t octet = 0;
        v = 0;
        for(;;)
        {
            const char c = *s++;
            if(c >= '0'  &&  c <= '9')
            {
                v = v * 10u + (uint32_t)(c - '0');
                if(v > 255u) { octet = 0; break; }
            }
            else if(c == '.'  ||  c == '\0')
            {
                if(octet >= 4) { octet = 0; break; }
                _scratch[octet++] = (uint8_t)v;
                v = 0;
                if(c == '\0')
                    break;
            }
            else { octet = 0; break; }
        }
        n = octet == 4 ? 4u : 0u;
        break;
    }
    case STORE_STR:
    {
        /* the rest of the line, the tokens joined by one space */
        uint8_t i;
        for(i = from; i < argc  &&  n < sizeof _scratch; ++i)
        {
            const char *p = argv[i];
            if(i > from  &&  n < sizeof _scratch)
                _scratch[n++] = ' ';
            while(*p != '\0'  &&  n < sizeof _scratch)
                _scratch[n++] = (uint8_t)*p++;
        }
        if(n > pO->maxLen)
            n = 0;
        break;
    }
    default:                                    /* blob: hex digits */
    {
        uint8_t hi = 0, digits = 0;
        while(*s != '\0'  &&  n < sizeof _scratch)
        {
            const char c = *s++;
            uint8_t d;
            if(c >= '0'  &&  c <= '9') d = (uint8_t)(c - '0');
            else if(c >= 'a'  &&  c <= 'f') d = (uint8_t)(c - 'a' + 10);
            else if(c >= 'A'  &&  c <= 'F') d = (uint8_t)(c - 'A' + 10);
            else { digits = 1; break; }
            if(++digits % 2u == 1u)
                hi = d;
            else
                _scratch[n++] = (uint8_t)((hi << 4) | d);
        }
        if(digits % 2u != 0  ||  n == 0  ||  n > pO->maxLen)
            n = 0;
        break;
    }
    }
    if(n == 0)
    {
        nosh_putsP(NOSH_STR("config: bad value for type "));
        putType(pO->type);
        nosh_putEol();
    }
    return n;
}

/* ---- the sub commands ------------------------------------------------------------- */

static void putLine(const store_obj_t *pO)
{
    putName(pO->id, 18);
    putType(pO->type);
    putValue(pO->type, store_valuePtr(pO), pO->len);
    if(pO->flags & STORE_F_DIRTY)
        nosh_putsP(NOSH_STR(" *"));
    nosh_putEol();
}


static void cmdList(void)
{
    uint8_t i;
    for(i = 0; i < store_count(); ++i)
        putLine(store_objAt(i));
}


static void cmdDiff(void)
{
    uint8_t i, changed = 0;
    for(i = 0; i < store_count(); ++i)
    {
        const store_obj_t *pO = store_objAt(i);
        uint8_t savedLen = 0;
        int res;
        if(!(pO->flags & STORE_F_DIRTY))
            continue;
        ++ changed;
        putName(pO->id, 18);
        res = store_getSaved(pO->id, _scratch, (uint8_t)sizeof _scratch, &savedLen);
        if(res == STORE_ERR_LEN)
            nosh_putsP(NOSH_STR("(long)"));
        else if(res != STORE_OK)
            nosh_putsP(NOSH_STR("(flash error)"));
        else
            putValue(pO->type, _scratch, savedLen);
        nosh_putsP(NOSH_STR(" -> "));
        putValue(pO->type, store_valuePtr(pO), pO->len);
        nosh_putEol();
    }
    if(changed == 0)
    {
        nosh_putsP(NOSH_STR("no changes"));
        nosh_putEol();
    }
}


static void cmdInfo(void)
{
    nosh_putsP(NOSH_STR("block "));
    nosh_putI32(store_activeBlock());
    nosh_putsP(NOSH_STR(" seq "));
    nosh_putU32(store_seq());
    nosh_putsP(NOSH_STR(", image "));
    nosh_putU32(store_imageSize());
    nosh_putsP(NOSH_STR(" of "));
    nosh_putU32(store_blockSize());
    nosh_putsP(NOSH_STR(" bytes, "));
    nosh_putU32(store_count());
    nosh_putsP(NOSH_STR(" objects"));
    nosh_putsP(store_isDirty() ? NOSH_STR(", unsaved changes") : NOSH_STR(", saved"));
    nosh_putEol();
}


static void cmdConfig(uint8_t argc, const char *const argv[])
{
    const store_obj_t *pO;
    int res;

    if(store_count() == 0)
    {
        putErr(NOSH_STR("store"), STORE_ERR_NOT_INIT);
        return;
    }
    if(argc == 1)
    {
        cmdList();
        return;
    }
    if(nosh_strEqP(argv[1], NOSH_STR("get"))  &&  argc == 3)
    {
        if((pO = findObj(argv[2])) != NULL)
            putLine(pO);
    }
    else if(nosh_strEqP(argv[1], NOSH_STR("set"))  &&  argc >= 4)
    {
        uint8_t n;
        if((pO = findObj(argv[2])) == NULL)
            return;
        n = parseValue(pO, argc, argv, 3);
        if(n == 0)
            return;
        res = store_set(pO->id, _scratch, n);
        if(res != STORE_OK)
            putErr(NOSH_STR("set"), res);
        else
            putLine(pO);
    }
    else if(nosh_strEqP(argv[1], NOSH_STR("diff"))  &&  argc == 2)
        cmdDiff();
    else if(nosh_strEqP(argv[1], NOSH_STR("save"))  &&  argc == 2)
    {
        res = store_save();
        if(res != STORE_OK)
            putErr(NOSH_STR("save"), res);
        else
        {
            nosh_putsP(NOSH_STR("saved, seq "));
            nosh_putU32(store_seq());
            nosh_putsP(NOSH_STR(" in block "));
            nosh_putI32(store_activeBlock());
            nosh_putEol();
        }
    }
    else if(nosh_strEqP(argv[1], NOSH_STR("revert"))  &&  argc == 2)
    {
        res = store_revert();
        if(res != STORE_OK)
            putErr(NOSH_STR("revert"), res);
        else
        {
            nosh_putsP(NOSH_STR("reverted"));
            nosh_putEol();
        }
    }
    else if(nosh_strEqP(argv[1], NOSH_STR("erase")))
    {
        if(argc != 3  ||  !nosh_strEqP(argv[2], NOSH_STR("yes")))
        {
            nosh_putsP(NOSH_STR("config erase yes: erases both blocks and every value"));
            nosh_putEol();
            return;
        }
        res = store_erase();
        if(res != STORE_OK)
            putErr(NOSH_STR("erase"), res);
        else
        {
            nosh_putsP(NOSH_STR("erased"));
            nosh_putEol();
        }
    }
    else if(nosh_strEqP(argv[1], NOSH_STR("info"))  &&  argc == 2)
        cmdInfo();
    else
    {
        nosh_putsP(NOSH_STR("usage: config [get <obj> | set <obj> <value> | diff | save | revert | erase yes | info]"));
        nosh_putEol();
    }
}

NOSH_CMD(store_cmd_config, "config", "the stored configuration: get, set, diff, save", cmdConfig);
