#ifdef _MSC_VER
# define _CRT_NONSTDC_NO_WARNINGS
# define _CRT_SECURE_NO_WARNINGS
#endif

#define LUA_LIB
#include <lua.h>
#include <lauxlib.h>
#include <stdint.h>
#include <string.h>


/* Lua utils */

#define return_self(L) { lua_settop(L, 1); return 1; }

#if LUA_VERSION_NUM < 502
#include <assert.h>

# define luaL_newlib(L,l) (lua_newtable(L), luaL_register(L,NULL,l))
# define luaL_setfuncs(L,l,n) (assert(n==0), luaL_register(L,NULL,l))
# define luaL_setmetatable(L, name) \
    (luaL_getmetatable((L), (name)), lua_setmetatable(L, -2))

static int relindex(int idx, int offset) {
    if (idx < 0 && idx > LUA_REGISTRYINDEX)
        return idx + offset;
    return idx;
}

static void lua_rawgetp(lua_State *L, int idx, const void *p) {
    lua_pushlightuserdata(L, (void*)p);
    lua_rawget(L, relindex(idx, 1));
}

static void lua_rawsetp(lua_State *L, int idx, const void *p) {
    lua_pushlightuserdata(L, (void*)p);
    lua_insert(L, -2);
    lua_rawset(L, relindex(idx, 1));
}

static lua_Integer lua_tointegerx(lua_State *L, int idx, int *isint) {
    lua_Integer i = lua_tointeger(L, idx);
    if (isint) *isint = (i != 0 || lua_type(L, idx) == LUA_TNUMBER);
    return i;
}
#endif

static int typeerror(lua_State *L, int idx, const char *type) {
    lua_pushfstring(L, "%s expected, got %s", type, luaL_typename(L, idx));
    return luaL_argerror(L, idx, lua_tostring(L, -1));
}

static void *testudata(lua_State *L, int idx, const void *type) {
    void *p = lua_touserdata(L, idx);
    if (p != NULL && lua_getmetatable(L, idx)) {
        lua_rawgetp(L, LUA_REGISTRYINDEX, type);
        if (!lua_rawequal(L, -2, -1))
            p = NULL;
        lua_pop(L, 2);
        return p;
    }
    return NULL;
}

static void *checkudata(lua_State *L, int idx, const void *type) {
    void *p = testudata(L, idx, type);
    if (p == NULL) typeerror(L, idx, (const char*)type);
    return p;
}

static lua_Integer posrelat(lua_Integer pos, size_t len) {
    if (pos >= 0) return pos;
    else if (0u - (size_t)pos > len) return 0;
    else return (lua_Integer)len + pos;
}

static int rangerelat(lua_Integer *i, lua_Integer *j, size_t len) {
    lua_Integer ni = posrelat(*i, len);
    lua_Integer nj = posrelat(*j, len);
    if (ni < 1) ni = 1;
    if (nj > (lua_Integer)len) nj = len;
    *i = ni, *j = nj;
    return ni < nj;
}


/* protobuf types */

/* wire type */
#define PB_WIRETYPES(X)      \
    X(VARINT,  "varint"     ) \
    X(64BIT,   "64bit"      ) \
    X(LENGTH,  "bytes"      ) \
    X(GSTART,  "startgroup" ) \
    X(GEND,    "endgroup"   ) \
    X(32BIT,   "32bit"      ) \

/* real types, must by ordered */
#define PB_TYPES(X) \
    X(bool)         \
    X(bytes)        \
    X(double)       \
    X(enum)         \
    X(fixed32)      \
    X(fixed64)      \
    X(float)        \
    X(group)        \
    X(int32)        \
    X(int64)        \
    X(message)      \
    X(sfixed32)     \
    X(sfixed64)     \
    X(sint32)       \
    X(sint64)       \
    X(string)       \
    X(uint32)       \
    X(uint64)       \

typedef enum pb_WireType {
#define X(t, name) PB_T##t,
    PB_WIRETYPES(X)
#undef  X
    PB_TWCOUNT
} pb_WireType;

typedef enum pb_Type {
#define X(t) PB_T##t,
    PB_TYPES(X)
#undef  X
    PB_TCOUNT
} pb_Type;

static const char *pb_wiretypes[] = {
#define X(t, name) name,
    PB_WIRETYPES(X)
#undef  X
};

static const char *pb_types[] = {
#define X(name) #name,
    PB_TYPES(X)
#undef  X
};

static int find_wiretype(const char *s) {
    int i;
    for (i = 0; i < PB_TWCOUNT; ++i) {
        if (strcmp(s, pb_wiretypes[i]) == 0)
            return i;
    }
    return -1;
}

static int find_type(const char *s) {
    size_t start = 0, end = PB_TCOUNT-1;
    if (s == NULL) return -1;
    while (start <= end) {
        size_t mid = (start + end) >> 1;
        int res = strcmp(s, pb_types[mid]);
        if (res == 0)
            return mid;
        else if (res > 0)
            start = mid + 1;
        else
            end = mid - 1;
    }
    return -1;
}


/* protobuf integer conversion */
/* from: protobuf -> Lua, to: Lua -> protobuf */

static int Lconv_fromint32(lua_State *L) {
    uint64_t n = (uint64_t)luaL_checkinteger(L, 1);
    n &= ((uint64_t)1 << 32) - 1;
    lua_pushinteger(L, (lua_Integer)((n ^ (1 << 31)) - (1 << 31)));
    return 1;
}

static int Lconv_touint32(lua_State *L) {
    uint32_t n = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int Lconv_fromsint32(lua_State *L) {
    uint32_t n = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (n >> 1) ^ -(int32_t)(n & 1));
    return 1;
}

static int Lconv_fromsint64(lua_State *L) {
    uint64_t n = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (n >> 1) ^ -(int64_t)(n & 1));
    return 1;
}

static int Lconv_tosint32(lua_State *L) {
    uint32_t n = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (n << 1) ^ (n >> 31));
    return 1;
}

static int Lconv_tosint64(lua_State *L) {
    uint64_t n = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (n << 1) ^ (n >> 63));
    return 1;
}

static int Lconv_fromfloat(lua_State *L) {
    union { uint32_t u32; float f; } u;
    u.u32 = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushnumber(L, (lua_Number)u.f);
    return 1;
}

static int Lconv_fromdouble(lua_State *L) {
    union { uint64_t u64; double d; } u;
    u.u64 = (uint64_t)luaL_checkinteger(L, 1);
    lua_pushnumber(L, (lua_Number)u.d);
    return 1;
}

static int Lconv_tofloat(lua_State *L) {
    union { uint32_t u32; float f; } u;
    u.f = (float)luaL_checknumber(L, 1);
    lua_pushinteger(L, (lua_Integer)u.u32);
    return 1;
}

static int Lconv_todouble(lua_State *L) {
    union { uint64_t u64; double d; } u;
    u.d = (double)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)u.u64);
    return 1;
}

LUALIB_API int luaopen_pb_conv(lua_State *L) {
    luaL_Reg libs[] = {
        { "toint32", Lconv_touint32 },
#define ENTRY(name) { #name, Lconv_##name }
        ENTRY(fromint32),
        ENTRY(touint32),
        ENTRY(fromsint32),
        ENTRY(fromsint64),
        ENTRY(tosint32),
        ENTRY(tosint64),
        ENTRY(tofloat),
        ENTRY(todouble),
        ENTRY(fromfloat),
        ENTRY(fromdouble),
#undef  ENTRY
        { NULL, NULL }
    };
    luaL_newlib(L, libs);
    return 1;
}


/* protobuf encode buffer */

typedef struct pb_Buffer {
    size_t used;
    size_t size;
    lua_State *L;
    char *buf;
    char init_buff[LUAL_BUFFERSIZE];
} pb_Buffer;

#define pb_addchar(buf, ch) ((buf)->buf[(buf)->used++] = (ch))

static void pb_initbuffer(pb_Buffer *buf, lua_State *L) {
    buf->used = 0;
    buf->size = LUAL_BUFFERSIZE;
    buf->L = L;
    buf->buf = buf->init_buff;
}

static void pb_resetbuffer(pb_Buffer *buf) {
    if (buf->buf != buf->init_buff) {
        lua_pushnil(buf->L);
        lua_rawsetp(buf->L, LUA_REGISTRYINDEX, buf);
    }
    pb_initbuffer(buf, buf->L);
}

static void pb_prepbuffer(pb_Buffer *buf, size_t need) {
    need += buf->used;
    if (need > buf->size) {
        void *newud;
        size_t newsize = LUAL_BUFFERSIZE;
        while (need > newsize)
            newsize *= 2;
        newud = lua_newuserdata(buf->L, newsize);
        memcpy(newud, buf->buf, buf->used);
        lua_rawsetp(buf->L, LUA_REGISTRYINDEX, buf);
        buf->buf = newud;
        buf->size = newsize;
    }
}

static void pb_addvarint(pb_Buffer *buf, uint64_t n) {
    pb_prepbuffer(buf, 10);
    do {
        int cur = n & 0x7F;
        n >>= 7;
        pb_addchar(buf, n != 0 ? cur | 0x80 : cur);
    } while (n != 0);
}

static void pb_addfixed32(pb_Buffer *buf, uint32_t n) {
    int i;
    pb_prepbuffer(buf, 4);
    for (i = 0; i < 4; ++i) {
        pb_addchar(buf, n & 0xFF);
        n >>= 8;
    }
}

static void pb_addfixed64(pb_Buffer *buf, uint64_t n) {
    int i;
    pb_prepbuffer(buf, 8);
    for (i = 0; i < 8; ++i) {
        pb_addchar(buf, n & 0xFF);
        n >>= 8;
    }
}

static void pb_addtag(pb_Buffer *buf, uint32_t tag, int wiretype) {
    pb_addvarint(buf, (uint32_t)((tag << 3) | (wiretype & 7)));
}

static const char pb_buftype[] = "pb.Buffer";
#define check_buffer(L,idx) ((pb_Buffer*)checkudata(L,idx,(const void*)pb_buftype))

static const char *pb_tolbuffer(lua_State *L, int idx, size_t *plen) {
    if (lua_type(L, idx) == LUA_TUSERDATA) {
        pb_Buffer *buf = check_buffer(L, idx);
        if (plen) *plen = buf->used;
        return buf->buf;
    }
    return luaL_checklstring(L, idx, plen);
}

static int Lbuf_tostring(lua_State *L) {
    pb_Buffer *buf = (pb_Buffer*)testudata(L, 1, pb_buftype);
    if (buf != NULL)
        lua_pushfstring(L, "pb.Buffer: %p", buf);
    else
        luaL_tolstring(L, 1, NULL);
    return 1;
}

static int Lbuf_new(lua_State *L) {
    int i, top = lua_gettop(L);
    pb_Buffer *buf = (pb_Buffer*)lua_newuserdata(L, sizeof(pb_Buffer));
    pb_initbuffer(buf, L);
    lua_rawgetp(L, LUA_REGISTRYINDEX, pb_buftype);
    lua_setmetatable(L, -2);
    for (i = 1; i <= top; ++i) {
        size_t len;
        const char *s = pb_tolbuffer(L, i, &len);
        pb_prepbuffer(buf, len);
        memcpy(&buf->buf[buf->used], s, len);
        buf->used += len;
    }
    return 1;
}

static int Lbuf_reset(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    pb_resetbuffer(buf);
    return_self(L);
}

static int Lbuf_len(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    lua_pushinteger(L, (lua_Integer)buf->used);
    return 1;
}

static int Lbuf_tag(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    lua_Integer tag = luaL_checkinteger(L, 2);
    int isint, wiretype = (int)lua_tointegerx(L, 3, &isint);
    if (!isint && (wiretype = find_wiretype(luaL_checkstring(L, 3)) < 0))
        return luaL_argerror(L, 3, "invalid wire type name");
    if (tag < 0 || tag > (1<<29))
        luaL_argerror(L, 2, "tag too big");
    pb_addtag(buf, tag, wiretype);
    return_self(L);
}

static int Lbuf_varint(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    int i, top = lua_gettop(L);
    for (i = 2; i <= top; ++i) {
        lua_Integer n = luaL_checkinteger(L, i);
        pb_addvarint(buf, n);
    }
    return_self(L);
}

static int Lbuf_bytes(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    int i, top = lua_gettop(L);
    for (i = 2; i <= top; ++i) {
        size_t len;
        const char *s = pb_tolbuffer(L, i, &len);
        pb_addvarint(buf, len);
        pb_prepbuffer(buf, len);
        memcpy(&buf->buf[buf->used], s, len);
        buf->used += len;
    }
    return_self(L);
}

static int Lbuf_fixed32(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    int i, top = lua_gettop(L);
    for (i = 2; i <= top; ++i) {
        uint32_t n =  (uint32_t)luaL_checkinteger(L, i);
        pb_addfixed32(buf, n);
    }
    return_self(L);
}

static int Lbuf_fixed64(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    int i, top = lua_gettop(L);
    for (i = 2; i <= top; ++i) {
        uint64_t n =  (uint64_t)luaL_checkinteger(L, i);
        pb_addfixed64(buf, n);
    }
    return_self(L);
}

static int Lbuf_add(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    uint32_t tag = 0, hastag = 0;
    const char *s, *type = luaL_checkstring(L, 3);
    union { float f; uint32_t u32;
            double d; uint64_t u64; } u;
    if (!lua_isnoneornil(L, 2)) {
        tag = (uint32_t)luaL_checkinteger(L, 2);
        hastag = 1;
    }
    switch (find_type(type)) {
    case PB_Tbool:
        u.u32 = lua_toboolean(L, 4);
        if (hastag) pb_addtag(buf, tag, PB_TVARINT);
        pb_prepbuffer(buf, 1);
        pb_addchar(buf, u.u32 ? 1 : 0);
        break;
    case PB_Tbytes:
    case PB_Tstring:
    case PB_Tmessage:
        s = luaL_checklstring(L, 4, &u.u32);
        if (hastag) pb_addtag(buf, tag, PB_TLENGTH);
        pb_addvarint(buf, u.u32);
        pb_prepbuffer(buf, u.u32);
        memcpy(&buf->buf[buf->used], s, u.u32);
        buf->used += u.u32;
        break;
    case PB_Tdouble:
        u.d = (double)luaL_checknumber(L, 4);
        if (hastag) pb_addtag(buf, tag, PB_T64BIT);
        pb_addfixed64(buf, u.u64);
        break;
    case PB_Tfloat:
        u.f = (float)luaL_checknumber(L, 4);
        if (hastag) pb_addtag(buf, tag, PB_T32BIT);
        pb_addfixed32(buf, u.u32);
        break;
    case PB_Tfixed32:
        u.u32 = (uint32_t)luaL_checkinteger(L, 4);
        if (hastag) pb_addtag(buf, tag, PB_T32BIT);
        pb_addfixed32(buf, u.u32);
        break;
    case PB_Tfixed64:
        u.u64 = (uint64_t)luaL_checkinteger(L, 4);
        if (hastag) pb_addtag(buf, tag, PB_T64BIT);
        pb_addfixed64(buf, u.u64);
        break;
    case PB_Tint32:
    case PB_Tuint32:
        u.u32 = (uint32_t)luaL_checkinteger(L, 4);
        if (hastag) pb_addtag(buf, tag, PB_TVARINT);
        pb_addvarint(buf, (uint64_t)u.u32);
        break;
    case PB_Tenum:
    case PB_Tint64:
    case PB_Tuint64:
        u.u64 = (uint64_t)luaL_checkinteger(L, 4);
        if (hastag) pb_addtag(buf, tag, PB_TVARINT);
        pb_addvarint(buf, u.u64);
        break;
    case PB_Tsint32:
        u.u32 = (uint32_t)luaL_checkinteger(L, 4);
        u.u32 = (u.u32 << 1) ^ (u.u32 >> 31);
        if (hastag) pb_addtag(buf, tag, PB_TVARINT);
        pb_addvarint(buf, (uint64_t)u.u32);
        break;
    case PB_Tsint64:
        u.u64 = (uint64_t)luaL_checkinteger(L, 4);
        u.u64 = (u.u64 << 1) ^ (u.u64 >> 63);
        if (hastag) pb_addtag(buf, tag, PB_TVARINT);
        pb_addvarint(buf, u.u64);
        break;
    case PB_Tgroup:
    default:
        lua_pushfstring(L, "unknown type '%s'", type);
        return luaL_argerror(L, 3, lua_tostring(L, -1));
    }
    return_self(L);
}

static int Lbuf_clear(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    size_t sz = (size_t)luaL_optinteger(L, 2, buf->used);
    if (sz > buf->used) sz = buf->used;
    buf->used -= sz;
    if (lua_toboolean(L, 3)) {
        lua_pushlstring(L, &buf->buf[buf->used], sz);
        return 1;
    }
    return_self(L);
}

static int Lbuf_concat(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    int i, top = lua_gettop(L);
    for (i = 2; i <= top; ++i) {
        size_t len;
        const char *s = pb_tolbuffer(L, i, &len);
        pb_prepbuffer(buf, len);
        memcpy(&buf->buf[buf->used], s, len);
        buf->used += len;
    }
    return_self(L);
}

static int Lbuf_result(lua_State *L) {
    pb_Buffer *buf = check_buffer(L, 1);
    const char *s = luaL_optstring(L, 2, NULL);
    if (s == NULL)
        lua_pushlstring(L, buf->buf, buf->used);
    else if (strcmp(s, "hex") == 0) {
        const char *hexa = "0123456789ABCDEF";
        luaL_Buffer b;
        char hex[4] = "XX ";
        size_t i;
        luaL_buffinit(L, &b);
        for (i = 0; i < buf->used; ++i) {
            hex[0] = hexa[(buf->buf[i]>>4)&0xF];
            hex[1] = hexa[(buf->buf[i]   )&0xF];
            if (i == buf->used-1) hex[2] = '\0';
            luaL_addstring(&b, hex);
        }
        luaL_pushresult(&b);
    }
    else {
        luaL_argerror(L, 2, "invalid options");
    }
    return 1;
}

LUALIB_API int luaopen_pb_buffer(lua_State *L) {
    luaL_Reg libs[] = {
        { "__gc", Lbuf_reset },
        { "__len", Lbuf_len },
        { "__concat", Lbuf_concat },
        { "__tostring", Lbuf_tostring },
#define ENTRY(name) { #name, Lbuf_##name }
        ENTRY(new),
        ENTRY(reset),
        ENTRY(tag),
        ENTRY(varint),
        ENTRY(bytes),
        ENTRY(fixed32),
        ENTRY(fixed64),
        ENTRY(add),
        ENTRY(clear),
        ENTRY(result),
        ENTRY(concat),
        ENTRY(len),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, pb_buftype)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, pb_buftype);
    }
    return 1;
}


/* protobuf decoder */

typedef struct pb_Decoder {
    size_t len;
    const char *s;
    const char *p, *end;
} pb_Decoder;

static int pb_readvarint(pb_Decoder *dec, uint64_t *pv) {
    uint64_t n = 0;
    const char *p = dec->p, *end;
    while (p < dec->end && (*p & 0x80) != 0)
        ++p;
    if (p >= dec->end)
        return 0;
    end = p + 1;
    while (p >= dec->p) {
        n <<= 7;
        n |= *p-- & 0x7F;
    }
    dec->p = end;
    *pv = n;
    return 1;
}

static int pb_readfixed32(pb_Decoder *dec, uint32_t *pv) {
    int i;
    uint32_t n = 0;
    if (dec->p + 4 > dec->end)
        return 0;
    for (i = 3; i >= 0; --i) {
        n <<= 8;
        n |= dec->p[i] & 0xFF;
    }
    *pv = n;
    return 1;
}

static int pb_readfixed64(pb_Decoder *dec, uint64_t *pv) {
    int i;
    uint64_t n = 0;
    if (dec->p + 8 < dec->end)
        return 0;
    for (i = 7; i >= 0; --i) {
        n <<= 8;
        n |= dec->p[i] & 0xFF;
    }
    *pv = n;
    return 1;
}

static int pb_skipvarint(pb_Decoder *dec) {
    const char *p = dec->p;
    while (p < dec->end && (*p & 0x80) != 0)
        ++p;
    if (p >= dec->end)
        return 0;
    dec->p = p + 1;
    return 1;
}

static int pb_skipsize(pb_Decoder *dec, size_t len) {
    if (dec->p + len > dec->end)
        return 0;
    dec->p += len;
    return 1;
}

static const char pb_decoder[]  = "pb.Decoder";
#define check_decoder(L,idx) ((pb_Decoder*)checkudata(L,idx,(const void*)pb_decoder))

typedef struct pb_FBDecoder {
    pb_Decoder *dec;
    const char *fb;
    lua_State *L;
} pb_FBDecoder;

static pb_FBDecoder check_fbdecoder(lua_State *L, int idx) {
    pb_FBDecoder dec;
    dec.dec = check_decoder(L, idx);
    dec.fb = dec.dec->p;
    dec.L = L;
    return dec;
}

static int restore_decoder(pb_FBDecoder *dec) {
    dec->dec->p = dec->fb;
    return 0;
}

static int type_mismatch(pb_FBDecoder *dec, int type, const char *wt) {
    /* assert(type >= 0 && type < PB_TCOUNT); */
    restore_decoder(dec);
    return luaL_error(dec->L, "can not convert from %s to %s",
            wt, pb_types[type]);
}

static int pb_pushvarint(pb_FBDecoder *dec, int type) {
    uint64_t n;
    lua_Integer out;
    if (!pb_readvarint(dec->dec, &n)) return 0;
    switch (type) {
    case PB_Tint32:
        n &= ((uint64_t)1 << 32) - 1;
        out = (lua_Integer)((n ^ (1 << 31)) - (1 << 31));
        break;
    case PB_Tuint32:
        out = (lua_Integer)(n & ~(uint32_t)0);
        break;
    case PB_Tsint32:
        out = (lua_Integer)((n >> 1) ^ -(int32_t)(n & 1));
        break;
    case PB_Tsint64:
        out = (lua_Integer)((n >> 1) ^ -(int64_t)(n & 1));
        break;
    case -1:
    case PB_Tint64:
    case PB_Tuint64:
    case PB_Tenum:
        out = (lua_Integer)n;
        break;
    case PB_Tbool:
        lua_pushboolean(dec->L, n != (lua_Integer)0);
        return 1;
    default: return type_mismatch(dec, type, "varint");
    }
    lua_pushinteger(dec->L, out);
    return 1;
}

static int pb_pushfixed32(pb_FBDecoder *dec, int type) {
    union { uint32_t u32; float f; } u;
    lua_Integer out;
    if (!pb_readfixed32(dec->dec, &u.u32)) return 0;
    switch (type) {
    case -1:
    case PB_Tfixed32:
        out = (lua_Integer)u.u32;
        return 1;
    case PB_Tfloat:
        lua_pushnumber(dec->L, (lua_Number)u.f);
        return 1;
    case PB_Tsfixed32:
        out = (lua_Integer)u.u32;
        if (sizeof(out) > 4)
            out &= ((uint64_t)1 << 32) - 1;
        out = (lua_Integer)((out ^ (1 << 31)) - (1 << 31));
        break;
    default: return type_mismatch(dec, type, "fixed32");
    }
    lua_pushinteger(dec->L, out);
    return 1;
}

static int pb_pushfixed64(pb_FBDecoder *dec, int type) {
    union { uint64_t u64; double d; } u;
    lua_Integer out;
    if (!pb_readfixed64(dec->dec, &u.u64)) return 0;
    switch (type) {
    case PB_Tdouble:
        lua_pushnumber(dec->L, (lua_Number)u.d);
        return 1;
    case -1:
    case PB_Tfixed64:
    case PB_Tsfixed64:
        out = (lua_Integer)u.u64;
        return 1;
    default: return type_mismatch(dec, type, "fixed64");
    }
    lua_pushinteger(dec->L, out);
    return 1;
}

static int pb_pushscalar(pb_FBDecoder *dec, int wiretype, int type) {
    uint64_t n;
    switch (wiretype) {
    case PB_TVARINT:
        return pb_pushvarint(dec, type);
    case PB_T64BIT:
        return pb_pushfixed64(dec, type);
    case PB_TLENGTH:
#if 0 /* not enabled */
        if (type >= 0 && (type != PB_Tbytes
                      ||  type != PB_Tstring
                      ||  type != PB_Tmessage)) {
            restore_decoder(dec);
            return luaL_error(dec->L, "read string with invalid type: %s",
                    pb_types[type]);
        }
#endif
        if (!pb_readvarint(dec->dec, &n)) return 0;
        if (dec->dec->end - dec->dec->p < n) return 0;
        lua_pushlstring(dec->L, dec->dec->p, (size_t)n);
        dec->dec->p += n;
        return 1;
    case PB_T32BIT:
        return pb_pushfixed32(dec, type);
    case PB_TGSTART: /* start group */
    case PB_TGEND: /* end group */ /* XXX groups unimplement */
    default:
        restore_decoder(dec);
        return luaL_error(dec->L,
                "unsupported wire type: %d", wiretype);
    }
}

static void init_decoder(pb_Decoder *dec, lua_State *L, int idx) {
    size_t len;
    const char *s = pb_tolbuffer(L, idx, &len);
    lua_Integer i = luaL_optinteger(L, idx+1, 1);
    lua_Integer j = luaL_optinteger(L, idx+2, len);
    rangerelat(&i, &j, len);
    dec->s = s;
    dec->len = len;
    dec->p = s + i - 1;
    dec->end = dec->p + j;
    lua_pushvalue(L, idx);
    lua_rawsetp(L, LUA_REGISTRYINDEX, dec);
}

static int Ldec_tostring(lua_State *L) {
    pb_Decoder *dec = (pb_Decoder*)testudata(L, 1, pb_decoder);
    if (dec != NULL)
        lua_pushfstring(L, "pb.Decoder: %p", dec);
    else
        luaL_tolstring(L, 1, NULL);
    return 1;
}

static int Ldec_new(lua_State *L) {
    pb_Decoder *dec;
    if (lua_gettop(L) == 0) {
        dec = (pb_Decoder*)lua_newuserdata(L, sizeof(pb_Decoder));
        dec->len = 0;
        dec->s = dec->p = dec->end = NULL;
    }
    else {
        lua_settop(L, 3);
        dec = (pb_Decoder*)lua_newuserdata(L, sizeof(pb_Decoder));
        init_decoder(dec, L, 1);
    }
    lua_rawgetp(L, LUA_REGISTRYINDEX, pb_decoder);
    lua_setmetatable(L, -2);
    return 1;
}

static int Ldec_reset(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    lua_pushnil(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, dec);
    dec->len = 0;
    dec->s = dec->p = dec->end = NULL;
    return 0;
}

static int Ldec_source(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    size_t oi = dec->p - dec->s + 1;
    size_t oj = dec->end - dec->s;
    int top = lua_gettop(L);
    if (top != 1) lua_settop(L, 3);
    lua_rawgetp(L, LUA_REGISTRYINDEX, dec);
    lua_pushinteger(L, oi);
    lua_pushinteger(L, oj);
    if (top != 1) init_decoder(dec, L, 2);
    return 3;
}

static int Ldec_pos(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    size_t pos = dec->p - dec->s + 1;
    lua_pushinteger(L, (lua_Integer)pos);
    if (lua_gettop(L) != 1) {
        lua_Integer npos = posrelat(luaL_optinteger(L, 2, pos),
                dec->end - dec->s);
        if (npos < 1) npos = 1;
        dec->p = dec->s + npos - 1;
    }
    return 1;
}

static int Ldec_len(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    int type = lua_type(L, 2);
    lua_pushinteger(L, (lua_Integer)(dec->end - dec->s));
    lua_pushinteger(L, (lua_Integer)dec->len);
    if (type <= 0)
        dec->end = dec->s + dec->len;
    else if (type == LUA_TNUMBER) {
        size_t len = (size_t)lua_tointeger(L, 2);
        if (len > dec->len) len = dec->len;
        dec->end = dec->s + len;
    }
    return 2;
}

static int Ldec_finished(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    lua_pushboolean(L, dec->p >= dec->end);
    return 1;
}

static int Ldec_tag(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    uint64_t n = 0;
    int wiretype;
    if (!pb_readvarint(dec, &n)) return 0;
    wiretype = (int)(n & 0x7);
    lua_pushinteger(L, (lua_Integer)(n >> 3));
    lua_pushinteger(L, (lua_Integer)wiretype);
    if (wiretype >= 0 && wiretype < PB_TWCOUNT) {
        lua_pushstring(L, pb_wiretypes[wiretype]);
        return 3;
    }
    return 2;
}

static int Ldec_varint(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    uint64_t n = 0;
    if (!pb_readvarint(dec, &n)) return 0;
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int Ldec_fixed32(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    uint32_t n = 0;
    if (!pb_readfixed32(dec, &n)) return 0;
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int Ldec_fixed64(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    uint64_t n = 0;
    if (!pb_readfixed64(dec, &n)) return 0;
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int Ldec_bytes(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    const char *p = dec->p;
    uint64_t n = (uint64_t)luaL_optinteger(L, 2, 0);
    if (n == 0 && !pb_readvarint(dec, &n))
        return 0;
    if (dec->end - dec->p < n) {
        dec->p = p;
        return 0;
    }
    lua_pushlstring(L, dec->p, (size_t)n);
    dec->p += n;
    return 1;
}

static int get_wiretype(lua_State *L, pb_Decoder *dec,
        int idx, int *wiretype) {
    uint64_t n;
    switch (lua_type(L, idx)) {
    case LUA_TNIL: case LUA_TNONE:
        if (!pb_readvarint(dec, &n)) return -1;
        lua_pushinteger(L, (lua_Integer)(n >> 3));
        *wiretype = n & 0x7;
        return 1;
    case LUA_TNUMBER:
        *wiretype = (int)lua_tointeger(L, idx);
        return 0;
    case LUA_TSTRING:
        if ((*wiretype = find_wiretype(lua_tostring(L, idx))) < 0)
            luaL_argerror(L, idx, "invalid wire type name");
        return 0;
    default:
        return typeerror(L, idx, "nil/number/string");
    }
}

static int skipvalue(pb_FBDecoder *dec, int wiretype) {
    int res;
    uint64_t n;
    switch (wiretype) {
    case PB_TVARINT:
        res = pb_skipvarint(dec->dec); break;
    case PB_T64BIT:
        res = pb_skipsize(dec->dec, 8); break;
    case PB_TLENGTH:
        res = pb_readvarint(dec->dec, &n)
            && pb_skipsize(dec->dec, (size_t)n);
        break;
    case PB_T32BIT:
        res = pb_skipsize(dec->dec, 4);
        break;
    default:
    case PB_TGSTART: /* start group */
    case PB_TGEND: /* end group */
        /* TODO need implemenbt */
        restore_decoder(dec);
        luaL_error(dec->L, "group unsupport");
        return 0;
    }
    if (!res) restore_decoder(dec);
    return res;
}

static int Ldec_fetch(lua_State *L) {
    pb_FBDecoder dec = check_fbdecoder(L, 1);
    int type = find_type(luaL_optstring(L, 3, NULL));
    int wiretype, extra = get_wiretype(L, dec.dec, 2, &wiretype);
    if (extra >= 0 && pb_pushscalar(&dec, wiretype, type))
        return extra + 1;
    restore_decoder(&dec);
    return 0;
}

static int Ldec_skip(lua_State *L) {
    pb_FBDecoder dec = check_fbdecoder(L, 1);
    int wiretype, extra = get_wiretype(L, dec.dec, 2, &wiretype);
    if (extra >= 0 && skipvalue(&dec, wiretype)) {
        lua_pushinteger(L, wiretype);
        return extra + 1;
    }
    restore_decoder(&dec);
    return 0;
}

static int values_iter(lua_State *L) {
    pb_FBDecoder dec = check_fbdecoder(L, 1);
    uint64_t n;
    if (dec.dec->p >= dec.dec->end)
        return 0;
    if (!pb_readvarint(dec.dec, &n))
        return luaL_error(L, "incomplete proto messages");
    lua_pushinteger(L, (lua_Integer)(n >> 3));
    if (pb_pushscalar(&dec, n & 0x7, -1))
        return 2;
    restore_decoder(&dec);
    return 0;
}

static int Ldec_values(lua_State *L) {
    check_decoder(L, 1);
    lua_pushcfunction(L, values_iter);
    lua_pushvalue(L, 1);
    return 2;
}

static int Ldec_update(lua_State *L) {
    pb_Decoder *dec = check_decoder(L, 1);
    pb_Buffer *buf;
    lua_rawgetp(L, LUA_REGISTRYINDEX, dec);
    if ((buf = testudata(L, -1, pb_buftype)) == NULL)
        return 0;
    if (buf->used == dec->p - dec->s) {
        dec->p = dec->s;
        buf->used = 0;
    }
    dec->p = buf->buf + (dec->p - dec->s);
    dec->s = buf->buf;
    dec->len = buf->used;
    dec->end = buf->buf + buf->used;
    return_self(L);
}

LUALIB_API int luaopen_pb_decoder(lua_State *L) {
    luaL_Reg libs[] = {
        { "__gc", Ldec_reset },
        { "__len", Ldec_len },
        { "__tostring", Ldec_tostring },
#define ENTRY(name) { #name, Ldec_##name }
        ENTRY(new),
        ENTRY(reset),
        ENTRY(source),
        ENTRY(pos),
        ENTRY(len),
        ENTRY(tag),
        ENTRY(bytes),
        ENTRY(fixed32),
        ENTRY(fixed64),
        ENTRY(varint),
        ENTRY(fetch),
        ENTRY(skip),
        ENTRY(values),
        ENTRY(finished),
        ENTRY(update),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, pb_decoder)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, pb_decoder);
    }
    return 1;
}

/* io routines */

#ifdef _WIN32
# include <io.h>
# include <fcntl.h>
#else
# define setmode(a,b)  ((void)0)
#endif

static int io_write(lua_State *L, FILE *f, int arg) {
    int nargs = lua_gettop(L) - arg + 1;
    int status = 1;
    for (; nargs--; arg++) {
        size_t l;
        const char *s = pb_tolbuffer(L, arg, &l);
        status = status && (fwrite(s, sizeof(char), l, f) == l);
    }
    if (status) return 1;  /* file handle already on stack top */
    else return luaL_fileresult(L, status, NULL);
}

static int Lio_read(lua_State *L) {
    const char *fname = luaL_optstring(L, 1, NULL);
    luaL_Buffer b;
    FILE *fp = stdin;
    size_t nr;
    if (fname == NULL)
        setmode(fileno(stdin), O_BINARY);
    else if ((fp = fopen(fname, "rb")) == NULL)
        return luaL_fileresult(L, 0, fname);
    luaL_buffinit(L, &b);
    do {  /* read file in chunks of LUAL_BUFFERSIZE bytes */
        char *p = luaL_prepbuffer(&b);
        nr = fread(p, sizeof(char), LUAL_BUFFERSIZE, fp);
        luaL_addsize(&b, nr);
    } while (nr == LUAL_BUFFERSIZE);
    if (fp != stdin) fclose(fp);
    else setmode(fileno(stdin), O_TEXT);
    luaL_pushresult(&b);  /* close buffer */
    return 1;
}

static int Lio_write(lua_State *L) {
    int res;
    setmode(fileno(stdout), O_BINARY);
    res = io_write(L, stdout, 1);
    fflush(stdout);
    setmode(fileno(stdout), O_TEXT);
    return res;
}

static int Lio_dump(lua_State *L) {
    int res;
    const char *fname = luaL_checkstring(L, 1);
    FILE *fp = fopen(fname, "wb");
    if (fp == NULL) return luaL_fileresult(L, 0, fname);
    res = io_write(L, fp, 1);
    fclose(fp);
    return res;
}

LUALIB_API int luaopen_pb_io(lua_State *L) {
    luaL_Reg libs[] = {
#define ENTRY(name) { #name, Lio_##name }
        ENTRY(read),
        ENTRY(write),
        ENTRY(dump),
#undef  ENTRY
        { NULL, NULL }
    };
    luaL_newlib(L, libs);
    return 1;
}

/* cc: flags+='-s -O3 -mdll -DLUA_BUILD_AS_DLL'
 * xcc: flags+='-ID:\luajit\include' libs+='-LD:\luajit\'
 * cc: output='pb.dll' libs+='-llua53' */
