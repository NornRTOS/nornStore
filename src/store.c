/**
 * @file store.c
 * The store: the object table and buffer the application gave it, the two blocks, the
 * load at init, get/set in RAM, and the A/B save. Everything that touches a block goes
 * through small read/write helpers so that the driver sees aligned, bounded accesses.
 */

#include "store_internal.h"

/* ---- state ---------------------------------------------------------------------- */

static const store_flash_t *_pFlash = NULL;
static store_obj_t *_pObjs = NULL;
static uint8_t _count = 0;
static uint8_t *_pBuf = NULL;
static int8_t _active = -1;             /* block holding the image, -1 none */
static uint32_t _seq = 0;               /* its sequence number, 0 none */
static uint16_t _activeLen = 0;         /* bytes of records in it */
static rtos_mutex_t _mutex;
static void (*_onChange)(uint16_t id, void *arg) = NULL;
static void *_onChangeArg = NULL;

/* ---- little helpers ------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

/** The lock is needed once tasks run; in main() before rtos_start() there is one context
    and the kernel would refuse a lock from it. */
static void lock(void)
{
    if(rtos_getCurrentTask() != 0)
        rtos_mutexLock(&_mutex);
}

static void unlock(void)
{
    if(rtos_getCurrentTask() != 0)
        rtos_mutexUnlock(&_mutex);
}

static store_obj_t *find(uint16_t id)
{
    uint8_t i;
    for(i = 0; i < _count; ++i)
        if(_pObjs[i].id == id)
            return &_pObjs[i];
    return NULL;
}

/** Size of the fixed types; 0 for the variable ones. */
static uint8_t fixedSize(uint8_t type)
{
    switch(type)
    {
    case STORE_U8: case STORE_BOOL:                 return 1;
    case STORE_U16:                                 return 2;
    case STORE_U32: case STORE_I32: case STORE_IP4: return 4;
    default:                                        return 0;
    }
}

static bool blockRead(uint8_t block, uint16_t off, void *pBuf, size_t n)
{
    return _pFlash->read((uint32_t)block * _pFlash->blockSize + off, pBuf, n);
}

/* ---- reading a block ------------------------------------------------------------ */

/** Validates block's header and CRC; returns true and fills seq and len if it is an
    image. Reads the records in small chunks: no block-sized buffer anywhere. */
static bool blockValid(uint8_t block, uint32_t *pSeq, uint16_t *pLen)
{
    uint8_t hdr[STORE_HEADER_SIZE];
    uint8_t chunk[16];
    uint16_t len, off, crc = 0xFFFFu;

    if(!blockRead(block, 0, hdr, sizeof hdr))
        return false;
    if(rd32(hdr) != STORE_MAGIC)
        return false;
    len = rd16(hdr + 8);
    if(len > _pFlash->blockSize - STORE_HEADER_SIZE  ||  (len & 3u) != 0)
        return false;
    for(off = 0; off < len; off = (uint16_t)(off + sizeof chunk))
    {
        const uint16_t n = (uint16_t)(len - off) < sizeof chunk ? (uint16_t)(len - off) : (uint16_t)sizeof chunk;
        if(!blockRead(block, (uint16_t)(STORE_HEADER_SIZE + off), chunk, n))
            return false;
        crc = store_crc16(crc, chunk, n);
    }
    if(crc != rd16(hdr + 10))
        return false;
    *pSeq = rd32(hdr + 4);
    *pLen = len;
    return true;
}

/** Walks the records of the active block; for each one calls fn(id, type, ver, len,
    offset-of-data) and stops when fn returns false. */
typedef bool (*recordFn_t)(uint16_t id, uint8_t type, uint8_t ver, uint8_t len, uint16_t dataOff, void *pArg);

static bool forEachRecord(recordFn_t fn, void *pArg)
{
    uint16_t off = STORE_HEADER_SIZE;
    const uint16_t end = (uint16_t)(STORE_HEADER_SIZE + _activeLen);
    while(off + 8u <= end)
    {
        uint8_t rec[8];
        if(!blockRead((uint8_t)_active, off, rec, sizeof rec))
            return false;
        if(off + STORE_RECORD_SIZE(rec[4]) > end)
            return false;                                   /* truncated: stop */
        if(!fn(rd16(rec), rec[2], rec[3], rec[4], (uint16_t)(off + 8u), pArg))
            return true;
        off = (uint16_t)(off + STORE_RECORD_SIZE(rec[4]));
    }
    return true;
}

static bool loadRecord(uint16_t id, uint8_t type, uint8_t ver, uint8_t len, uint16_t dataOff, void *pArg)
{
    store_obj_t *pO = find(id);
    (void)pArg;
    if(pO != NULL  &&  pO->type == type  &&  len <= pO->maxLen)
    {
        if(blockRead((uint8_t)_active, dataOff, &_pBuf[pO->offset], len))
        {
            pO->len = len;
            pO->verInFlash = ver;
            pO->flags = STORE_F_FROM_FLASH;
        }
    }
    return true;
}

/** Clears every value, then fills them from the active block if there is one. */
static int loadImage(void)
{
    uint8_t i;
    for(i = 0; i < _count; ++i)
    {
        _pObjs[i].len = 0;
        _pObjs[i].flags = 0;
        _pObjs[i].verInFlash = 0;
    }
    if(_active < 0)
        return STORE_OK;
    return forEachRecord(loadRecord, NULL) ? STORE_OK : STORE_ERR_FLASH;
}

/** Finds the image: the valid block with the highest sequence number. */
static int findActive(void)
{
    uint8_t b;
    _active = -1;
    _seq = 0;
    _activeLen = 0;
    for(b = 0; b < _pFlash->blocks; ++b)
    {
        uint32_t seq;
        uint16_t len;
        if(blockValid(b, &seq, &len)  &&  (_active < 0  ||  seq > _seq))
        {
            _active = (int8_t)b;
            _seq = seq;
            _activeLen = len;
        }
    }
    return STORE_OK;
}

/* ---- setup -------------------------------------------------------------------------- */

int store_init(const store_flash_t *pFlash, store_obj_t *pObjs, uint8_t count,
               uint8_t *pBuf, size_t bufSize)
{
    uint16_t offset = 0;
    uint8_t i;
    int res;

    _pFlash = NULL;
    if(pFlash == NULL  ||  pFlash->read == NULL  ||  pFlash->write == NULL  ||  pFlash->erase == NULL
       ||  pObjs == NULL  ||  pBuf == NULL  ||  count == 0  ||  pFlash->blocks < 2)
        return STORE_ERR_ARG;
    if(pFlash->writeAlign == 0  ||  (pFlash->blockSize % pFlash->writeAlign) != 0
       ||  pFlash->writeAlign > 4  ||  pFlash->blockSize < STORE_HEADER_SIZE + 8u)
        return STORE_ERR_LEN;
    /* find() searches the entries accepted so far, so a duplicate id is seen. */
    _pObjs = pObjs;
    _count = 0;
    for(i = 0; i < count; ++i)
    {
        const uint8_t fixed = fixedSize(pObjs[i].type);
        if(pObjs[i].type < STORE_U8  ||  pObjs[i].type > STORE_IP4)
            return STORE_ERR_TYPE;
        if(fixed != 0  &&  pObjs[i].maxLen != fixed)
            return STORE_ERR_LEN;
        if(find(pObjs[i].id) != NULL)
            return STORE_ERR_ID;
        pObjs[i].offset = offset;
        offset = (uint16_t)(offset + pObjs[i].maxLen);
        if(offset > bufSize)
            return STORE_ERR_LEN;
        _count = (uint8_t)(i + 1u);
    }
    _pFlash = pFlash;
    _pBuf = pBuf;
    rtos_mutexInit(&_mutex, STORE_CFG_MUTEX_CEILING);
    findActive();
    res = loadImage();
    if(res != STORE_OK)
        _pFlash = NULL;
    return res;
}

/* ---- the RAM image --------------------------------------------------------------- */

int store_get(uint16_t id, void *pOut, uint8_t maxLen, uint8_t *pLen)
{
    const store_obj_t *pO;
    uint8_t n;

    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    if(pOut == NULL)
        return STORE_ERR_ARG;
    pO = find(id);
    if(pO == NULL)
        return STORE_ERR_ID;
    lock();
    n = pO->len < maxLen ? pO->len : maxLen;
    {
        const uint8_t *src = &_pBuf[pO->offset];
        uint8_t i;
        for(i = 0; i < n; ++i)
            ((uint8_t *)pOut)[i] = src[i];
    }
    if(pLen != NULL)
        *pLen = pO->len;
    unlock();
    return pO->len <= maxLen ? STORE_OK : STORE_ERR_LEN;
}


int store_set(uint16_t id, const void *pData, uint8_t len)
{
    store_obj_t *pO;
    uint8_t fixed, i;

    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    if(pData == NULL  &&  len != 0)
        return STORE_ERR_ARG;
    pO = find(id);
    if(pO == NULL)
        return STORE_ERR_ID;
    if(len > pO->maxLen)
        return STORE_ERR_LEN;
    fixed = fixedSize(pO->type);
    if(fixed != 0  &&  len != fixed)
        return STORE_ERR_LEN;
    if(pO->type == STORE_BOOL  &&  *(const uint8_t *)pData > 1u)
        return STORE_ERR_VALUE;
#ifdef STORE_CFG_VALIDATE_HOOK
    {
        bool STORE_CFG_VALIDATE_HOOK(uint16_t, const void *, uint8_t);
        if(!STORE_CFG_VALIDATE_HOOK(id, pData, len))
            return STORE_ERR_VALUE;
    }
#endif
    lock();
    for(i = 0; i < len; ++i)
        _pBuf[pO->offset + i] = ((const uint8_t *)pData)[i];
    pO->len = len;
    pO->flags |= STORE_F_DIRTY;
    unlock();
    if(_onChange != NULL)
        _onChange(id, _onChangeArg);
    return STORE_OK;
}


void store_onChange(void (*fn)(uint16_t id, void *arg), void *arg)
{
    _onChange = fn;
    _onChangeArg = arg;
}


int store_getStr(uint16_t id, char *pOut, uint8_t size)
{
    const store_obj_t *pO = _pFlash != NULL ? find(id) : NULL;
    uint8_t len = 0;
    int res;

    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    if(pO == NULL)
        return STORE_ERR_ID;
    if(pO->type != STORE_STR)
        return STORE_ERR_TYPE;
    if(pOut == NULL  ||  size == 0)
        return STORE_ERR_ARG;
    res = store_get(id, pOut, (uint8_t)(size - 1u), &len);
    pOut[len < size ? len : (uint8_t)(size - 1u)] = '\0';
    return res;
}


int store_setStr(uint16_t id, const char *s)
{
    const store_obj_t *pO = _pFlash != NULL ? find(id) : NULL;
    size_t n = 0;

    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    if(pO == NULL)
        return STORE_ERR_ID;
    if(pO->type != STORE_STR)
        return STORE_ERR_TYPE;
    if(s == NULL)
        return STORE_ERR_ARG;
    while(s[n] != '\0')
        if(++n > 255u)
            return STORE_ERR_LEN;
    return store_set(id, s, (uint8_t)n);
}


static bool isNumber(uint8_t type)
{
    return type == STORE_U8  ||  type == STORE_U16  ||  type == STORE_U32  ||  type == STORE_I32
           ||  type == STORE_BOOL  ||  type == STORE_IP4;
}


int store_getU32(uint16_t id, uint32_t *pOut, bool *pSet)
{
    const store_obj_t *pO = _pFlash != NULL ? find(id) : NULL;
    uint8_t raw[4] = { 0, 0, 0, 0 };
    uint8_t len = 0;
    int res;

    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    if(pO == NULL)
        return STORE_ERR_ID;
    if(!isNumber(pO->type))
        return STORE_ERR_TYPE;
    if(pOut == NULL)
        return STORE_ERR_ARG;
    res = store_get(id, raw, sizeof raw, &len);
    if(res != STORE_OK)
        return res;
    if(pSet != NULL)
        *pSet = len != 0;
    if(pO->type == STORE_IP4)
        *pOut = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
    else
        *pOut = rd32(raw);
    return STORE_OK;
}


int store_setU32(uint16_t id, uint32_t value)
{
    const store_obj_t *pO = _pFlash != NULL ? find(id) : NULL;
    uint8_t raw[4];

    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    if(pO == NULL)
        return STORE_ERR_ID;
    if(!isNumber(pO->type))
        return STORE_ERR_TYPE;
    switch(pO->type)
    {
    case STORE_BOOL: if(value > 1u) return STORE_ERR_VALUE;      break;
    case STORE_U8:   if(value > 0xFFu) return STORE_ERR_VALUE;   break;
    case STORE_U16:  if(value > 0xFFFFu) return STORE_ERR_VALUE; break;
    default: break;
    }
    if(pO->type == STORE_IP4)
    {
        raw[0] = (uint8_t)(value >> 24); raw[1] = (uint8_t)(value >> 16);
        raw[2] = (uint8_t)(value >> 8);  raw[3] = (uint8_t)value;
    }
    else
        wr32(raw, value);
    return store_set(id, raw, fixedSize(pO->type));
}


/* ---- defaults --------------------------------------------------------------------- */

static bool fromFlash(uint16_t id)
{
    const store_obj_t * const pObj = store_objById(id);
    return pObj != NULL  &&  (pObj->flags & STORE_F_FROM_FLASH) != 0;
}

int store_default(uint16_t id, const void *pData, uint8_t len)
{
    return fromFlash(id) ? STORE_OK : store_set(id, pData, len);
}

int store_defaultStr(uint16_t id, const char *s)
{
    return fromFlash(id) ? STORE_OK : store_setStr(id, s);
}

int store_defaultU32(uint16_t id, uint32_t value)
{
    return fromFlash(id) ? STORE_OK : store_setU32(id, value);
}


int store_getI32(uint16_t id, int32_t *pOut, bool *pSet)
{
    uint32_t u;
    const int res = store_getU32(id, &u, pSet);
    if(res == STORE_OK)
        *pOut = (int32_t)u;
    return res;
}


int store_setI32(uint16_t id, int32_t value)
{
    const store_obj_t *pO = _pFlash != NULL ? find(id) : NULL;
    if(pO != NULL  &&  pO->type != STORE_I32)
        return STORE_ERR_TYPE;
    return store_setU32(id, (uint32_t)value);
}


bool store_isDirty(void)
{
    uint8_t i;
    for(i = 0; i < _count; ++i)
        if(_pObjs[i].flags & STORE_F_DIRTY)
            return true;
    return false;
}


int store_revert(void)
{
    int res;
    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    lock();
    res = loadImage();
    unlock();
    return res;
}

/* ---- flash -------------------------------------------------------------------------- */

/** Writes n bytes at off in block through the driver, in pieces that respect writeAlign:
    whole aligned words straight from the source, a last partial word padded with 0xFF.
    off is always aligned (the header is 12, records 8 + 4k). */
static bool blockWrite(uint8_t block, uint16_t off, const uint8_t *p, uint16_t n)
{
    const uint32_t base = (uint32_t)block * _pFlash->blockSize + off;
    const uint8_t a = _pFlash->writeAlign;
    const uint16_t whole = (uint16_t)(n - n % a);

    if(whole > 0  &&  !_pFlash->write(base, p, whole))
        return false;
    if(whole < n)
    {
        uint8_t tail[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        uint8_t i;
        for(i = 0; i < n - whole; ++i)
            tail[i] = p[whole + i];
        if(!_pFlash->write(base + whole, tail, a))
            return false;
    }
    return true;
}


uint16_t store_imageSize(void)
{
    uint16_t size = STORE_HEADER_SIZE;
    uint8_t i;
    for(i = 0; i < _count; ++i)
        if(_pObjs[i].len != 0)
            size = (uint16_t)(size + STORE_RECORD_SIZE(_pObjs[i].len));
    return size;
}


int store_save(void)
{
    uint8_t target, i;
    uint16_t off, len, crc = 0xFFFFu;
    uint8_t hdr[STORE_HEADER_SIZE];
    uint32_t seq;
    uint16_t checkLen;
    int res = STORE_OK;

    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    lock();
    if(store_imageSize() > _pFlash->blockSize)
    {
        unlock();
        return STORE_ERR_FULL;
    }
    target = (uint8_t)(_active < 0 ? 0 : (_active + 1) % _pFlash->blocks);

    if(!_pFlash->erase(target))
    {
        unlock();
        return STORE_ERR_FLASH;
    }

    /* The records, one by one: header, then data padded to 4. */
    off = STORE_HEADER_SIZE;
    for(i = 0; i < _count  &&  res == STORE_OK; ++i)
    {
        const store_obj_t *pO = &_pObjs[i];
        uint8_t rec[8];
        const uint8_t pad[3] = { 0xFF, 0xFF, 0xFF };
        const uint8_t padN = (uint8_t)((4u - pO->len % 4u) % 4u);
        if(pO->len == 0)
            continue;                                   /* never set: no record */
        wr16(rec, pO->id); rec[2] = pO->type; rec[3] = pO->ver; rec[4] = pO->len;
        rec[5] = rec[6] = rec[7] = 0xFF;
        crc = store_crc16(crc, rec, sizeof rec);
        crc = store_crc16(crc, &_pBuf[pO->offset], pO->len);
        crc = store_crc16(crc, pad, padN);
        if(!blockWrite(target, off, rec, sizeof rec)
           ||  !blockWrite(target, (uint16_t)(off + 8u), &_pBuf[pO->offset], pO->len))
            res = STORE_ERR_FLASH;
        off = (uint16_t)(off + STORE_RECORD_SIZE(pO->len));
    }
    len = (uint16_t)(off - STORE_HEADER_SIZE);

    /* The header last: it is what makes the block an image. */
    if(res == STORE_OK)
    {
        seq = _seq + 1u;
        wr32(hdr, STORE_MAGIC); wr32(hdr + 4, seq); wr16(hdr + 8, len); wr16(hdr + 10, crc);
        if(!blockWrite(target, 0, hdr, sizeof hdr))
            res = STORE_ERR_FLASH;
    }

    /* Read back: the block must now validate with exactly this sequence and length. */
    if(res == STORE_OK)
    {
        uint32_t checkSeq;
        if(!blockValid(target, &checkSeq, &checkLen)  ||  checkSeq != seq  ||  checkLen != len)
            res = STORE_ERR_VERIFY;
    }

    if(res == STORE_OK)
    {
        _active = (int8_t)target;
        _seq = seq;
        _activeLen = len;
        for(i = 0; i < _count; ++i)
        {
            _pObjs[i].flags &= (uint8_t)~STORE_F_DIRTY;
            if(_pObjs[i].len != 0)
            {
                _pObjs[i].flags |= STORE_F_FROM_FLASH;
                _pObjs[i].verInFlash = _pObjs[i].ver;
            }
        }
    }
    unlock();
    return res;
}


int store_erase(void)
{
    uint8_t b;
    int res = STORE_OK;
    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    lock();
    for(b = 0; b < _pFlash->blocks; ++b)
        if(!_pFlash->erase(b))
            res = STORE_ERR_FLASH;
    _active = -1;
    _seq = 0;
    _activeLen = 0;
    (void)loadImage();
    unlock();
    return res;
}

/* ---- introspection ------------------------------------------------------------------ */

uint8_t store_count(void) { return _pFlash != NULL ? _count : 0u; }

const store_obj_t *store_objAt(uint8_t index)
{
    return (_pFlash != NULL  &&  index < _count) ? &_pObjs[index] : NULL;
}

const store_obj_t *store_objById(uint16_t id)
{
    return _pFlash != NULL ? find(id) : NULL;
}

const uint8_t *store_valuePtr(const store_obj_t *pObj)
{
    return (_pFlash != NULL  &&  pObj != NULL) ? &_pBuf[pObj->offset] : NULL;
}

uint32_t store_seq(void) { return _seq; }
int8_t store_activeBlock(void) { return _active; }
uint16_t store_blockSize(void) { return _pFlash != NULL ? _pFlash->blockSize : 0u; }


typedef struct savedLookup
{
    uint16_t id;
    void *pOut;
    uint8_t maxLen;
    uint8_t len;
    bool found;
    bool ok;
} savedLookup_t;

static bool savedRecord(uint16_t id, uint8_t type, uint8_t ver, uint8_t len, uint16_t dataOff, void *pArg)
{
    savedLookup_t *pL = pArg;
    (void)type; (void)ver;
    if(id != pL->id)
        return true;
    pL->found = true;
    pL->len = len;
    pL->ok = blockRead((uint8_t)_active, dataOff, pL->pOut, len < pL->maxLen ? len : pL->maxLen);
    return false;
}

int store_getSaved(uint16_t id, void *pOut, uint8_t maxLen, uint8_t *pLen)
{
    savedLookup_t l = { 0, NULL, 0, 0, false, true };
    bool walked;

    if(_pFlash == NULL)
        return STORE_ERR_NOT_INIT;
    if(pOut == NULL)
        return STORE_ERR_ARG;
    if(find(id) == NULL)
        return STORE_ERR_ID;
    l.id = id; l.pOut = pOut; l.maxLen = maxLen;
    if(pLen != NULL)
        *pLen = 0;
    if(_active < 0)
        return STORE_OK;
    lock();
    walked = forEachRecord(savedRecord, &l);
    unlock();
    if(!walked  ||  !l.ok)
        return STORE_ERR_FLASH;
    if(pLen != NULL)
        *pLen = l.found ? l.len : 0u;
    return (l.found  &&  l.len > maxLen) ? STORE_ERR_LEN : STORE_OK;
}
