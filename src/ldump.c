/*
** $Id: ldump.c,v 2.8.1.1 2007/12/27 13:02:25 roberto Exp $
** save precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#include <stddef.h>

// clean C ...
#include <stdint.h>
#include <string.h>

#define ldump_c
#define LUA_CORE

#include "lua.h"

#include "lobject.h"
#include "lstate.h"
#include "lundump.h"

#include "ldo.h"
#include "lmem.h"

typedef struct {
 lua_State* L;
 lua_Writer writer;
 void* data;
 int strip;
 int status;
} DumpState;

#define DumpMem(b,n,size,D) DumpBlock(b,(n)*(size),D)
#define DumpVar(x,D)        DumpMem(&x,1,sizeof(x),D)

static void DumpBlock(const void* b, size_t size, DumpState* D)
{
 if (D->status==0)
 {
  lua_unlock(D->L);
  D->status=(*D->writer)(D->L,b,size,D->data);
  lua_lock(D->L);
 }
}

static void DumpChar(int y, DumpState* D)
{
 char x=(char)y;
 DumpVar(x,D);
}

static void DumpInt(int x, DumpState* D)
{
 DumpVar(x,D);
}

static void DumpNumber(lua_Number x, DumpState* D)
{
 DumpVar(x,D);
}

static void DumpVector(const void* b, int n, size_t size, DumpState* D)
{
 DumpInt(n,D);
 DumpMem(b,n,size,D);
}

static void DumpString(const TString* s, DumpState* D)
{
 if (s==NULL || getstr(s)==NULL)
 {
  size_t size=0;
  DumpVar(size,D);
 }
 else
 {
  size_t size=s->tsv.len+1;     /* include trailing '\0' */
  DumpVar(size,D);
  DumpBlock(getstr(s),size,D);
 }
}

#define DumpCode(f,D)    DumpVector(f->code,f->sizecode,sizeof(Instruction),D)

static void DumpFunction(const Proto* f, const TString* p, DumpState* D);

static void DumpConstants(const Proto* f, DumpState* D)
{
 int i,n=f->sizek;
 DumpInt(n,D);
 for (i=0; i<n; i++)
 {
  const TValue* o=&f->k[i];
  DumpChar(ttype(o),D);
  switch (ttype(o))
  {
   case LUA_TNIL:
    break;
   case LUA_TBOOLEAN:
    DumpChar(bvalue(o),D);
    break;
   case LUA_TNUMBER:
    DumpNumber(nvalue(o),D);
    break;
   case LUA_TSTRING:
    DumpString(rawtsvalue(o),D);
    break;
   default:
    lua_assert(0);          /* cannot happen */
    break;
  }
 }
 n=f->sizep;
 DumpInt(n,D);
 for (i=0; i<n; i++) DumpFunction(f->p[i],f->source,D);
}

static void DumpDebug(const Proto* f, DumpState* D)
{
 int i,n;
 n= (D->strip) ? 0 : f->sizelineinfo;
 DumpVector(f->lineinfo,n,sizeof(int),D);
 n= (D->strip) ? 0 : f->sizelocvars;
 DumpInt(n,D);
 for (i=0; i<n; i++)
 {
  DumpString(f->locvars[i].varname,D);
  DumpInt(f->locvars[i].startpc,D);
  DumpInt(f->locvars[i].endpc,D);
 }
 n= (D->strip) ? 0 : f->sizeupvalues;
 DumpInt(n,D);
 for (i=0; i<n; i++) DumpString(f->upvalues[i],D);
}

static void DumpFunction(const Proto* f, const TString* p, DumpState* D)
{
 DumpString((f->source==p || D->strip) ? NULL : f->source,D);
 DumpInt(f->linedefined,D);
 DumpInt(f->lastlinedefined,D);
 DumpChar(f->nups,D);
 DumpChar(f->numparams,D);
 DumpChar(f->is_vararg,D);
 DumpChar(f->maxstacksize,D);
 DumpCode(f,D);
 DumpConstants(f,D);
 DumpDebug(f,D);
}

#define BSWAP_32(x)     (((uint32_t)(x) << 24) | \
                        (((uint32_t)(x) <<  8)  & 0xff0000) | \
                        (((uint32_t)(x) >>  8)  & 0xff00) | \
                        ((uint32_t)(x)  >> 24))


#define BSWAP_64(x)     (((uint64_t)(x) << 56) | \
                        (((uint64_t)(x) << 40) & 0xff000000000000ULL) | \
                        (((uint64_t)(x) << 24) & 0xff0000000000ULL) | \
                        (((uint64_t)(x) << 8)  & 0xff00000000ULL) | \
                        (((uint64_t)(x) >> 8)  & 0xff000000ULL) | \
                        (((uint64_t)(x) >> 24) & 0xff0000ULL) | \
                        (((uint64_t)(x) >> 40) & 0xff00ULL) | \
                        ((uint64_t)(x)  >> 56))

static void (*dump_int)(int x, DumpState* D) = NULL;
static void (*dump_size_t)(size_t x, DumpState* D) = NULL;
static void (*dump_number)(lua_Number x, DumpState* D) = NULL;
static void (*dump_int_vector)(const void* b, int n, size_t size, DumpState* D) = NULL;
static void (*dump_code_vector)(const void* b, int n, DumpState* D) = NULL;

static void dump_int_o(int i, DumpState* D) {
    int32_t x = (int32_t)i;
    DumpBlock(&x, 4, D);
}

static void dump_int_s(int i, DumpState* D) {
    int32_t sx = (int32_t)i;
    sx = BSWAP_32(sx);
    DumpBlock(&sx, 4, D);
}

static void dump_size_t_o(size_t i, DumpState* D) {
    uint64_t sx = (uint64_t)i;
    DumpBlock(&sx, 8, D);
}

static void dump_size_t_s(size_t i, DumpState* D) {
    uint64_t sx = (uint64_t)i;
    sx = BSWAP_64(sx);
    DumpBlock(&sx, 8, D);
}

static void dump_number_o(lua_Number n, DumpState* D) {
    double dx = (double)n;
    DumpBlock(&dx, 8, D);
}

static void dump_number_s(lua_Number n, DumpState* D) {
    union {
        double d;
        uint64_t u;
    } du = {
        .d = (double)n
    };
    du.u = BSWAP_64(du.u);
    DumpBlock(&du.u, 8, D);
}

static void dump_string(const TString* s, DumpState* D) {
    if (s==NULL || getstr(s)==NULL) {
        size_t size = 0;
        dump_size_t(size,D);
    } else {
        size_t size=s->tsv.len+1;
        dump_size_t(size,D);
        DumpBlock(getstr(s),size,D);
    }
}

static void dump_int_vector_o(const void *b, int n, size_t size, DumpState* D) {
    dump_int_o(n, D);

    if (size == 4) {
        DumpBlock(b, n * 4, D);
    } else {
        const int * ip = (const int *)b;
        int32_t * d = luaM_newvector(D->L, n, int32_t);
        for (int i = 0; i < n; i++) {
            d[i] = ip[i];
        }
        DumpBlock(d, n * 4, D);
        luaM_freearray(D->L, d, n, int32_t);
    }
}

static void dump_int_vector_s(const void *b, int n, size_t size, DumpState* D) {
    const int * s = (const int *)b;
    int32_t * d = luaM_newvector(D->L, n, int32_t);

    dump_int_s(n, D);

    for (int i = 0; i < n; i++) {
        d[i] = BSWAP_32(s[i]);
    }

    DumpBlock(d, n * 4, D);
    luaM_freearray(D->L, d, n, int32_t);
}

static void dump_code_vector_o(const void *b, int n, DumpState* D) {
    dump_int_o(n, D);

    if (sizeof(Instruction) == 4) {
        DumpBlock(b, n * 4, D);
    } else {
        const Instruction * ip = (const Instruction *)b;
        uint32_t * d = luaM_newvector(D->L, n, uint32_t);
        for (int i = 0; i < n; i++) {
            d[i] = (uint32_t)ip[i];
        }
        DumpBlock(d, n * 4, D);
        luaM_freearray(D->L, d, n, uint32_t);
    }
}

static void dump_code_vector_s(const void *b, int n, DumpState* D) {
    const Instruction * s = (const Instruction *)b;
    int32_t * d = luaM_newvector(D->L, n, int32_t);
    dump_int_s(n, D);
    for (int i = 0; i < n; i++) {
        d[i] = BSWAP_32(s[i]);
    }
    DumpBlock(d, n * 4, D);
    luaM_freearray(D->L, d, n, int32_t);
}

static void setup_dump_funcs(DumpState* D) {
    int x = 0x1;
    int8_t ile = *(int8_t *)&x;
    if (dump_int == NULL) {
        dump_int = ile ? dump_int_o : dump_int_s;
    }
    if (dump_size_t == NULL) {
        dump_size_t = ile ? dump_size_t_o : dump_size_t_s;
    }
    if (dump_int_vector == NULL) {
        dump_int_vector = ile ? dump_int_vector_o : dump_int_vector_s;
    }
    if (dump_code_vector == NULL) {
        dump_code_vector = ile ? dump_code_vector_o : dump_code_vector_s;
    }

    if (dump_number == NULL) {
        double d = 1.2344999991522893623141499119810760021209716796875;
        if (memcmp(&d, "\x78\x56\x34\x12\x83\xC0\xF3\x3F", 8) == 0) {
            dump_number = dump_number_o;
        } else if (memcmp(&d, "\x3F\xF3\xC0\x83\x12\x34\x56\x78", 8) == 0) {
            dump_number = dump_number_s;
        } else {
            luaO_pushfstring(D->L,"dump: unknown number format");
            luaD_throw(D->L, LUA_ERRERR);
        }
    }
}

static void dump_function(const Proto* f, const TString* p, DumpState* D);

static void dump_constants(const Proto* f, DumpState* D) {
    int i,n=f->sizek;

    dump_int(n,D);

    for (i=0; i<n; i++) {
        const TValue* o=&f->k[i];
        DumpChar(ttype(o),D);
        switch (ttype(o)) {
            case LUA_TNIL:
                break;
            case LUA_TBOOLEAN:
                DumpChar(bvalue(o),D);
                break;
            case LUA_TNUMBER:
                dump_number(nvalue(o),D);
                break;
            case LUA_TSTRING:
                dump_string(rawtsvalue(o),D);
                break;
            default:
                lua_assert(0);          /* cannot happen */
                break;
        }
    }
    n=f->sizep;
    dump_int(n,D);
    for (i=0; i<n; i++) {
        dump_function(f->p[i], f->source, D);
    }
}

static void dump_debug(const Proto* f, DumpState* D) {
    int i,n;
    n= (D->strip) ? 0 : f->sizelineinfo;
    dump_int_vector(f->lineinfo, n, sizeof(int), D);
    n= (D->strip) ? 0 : f->sizelocvars;
    dump_int(n,D);
    for (i=0; i<n; i++) {
        dump_string(f->locvars[i].varname,D);
        dump_int(f->locvars[i].startpc,D);
        dump_int(f->locvars[i].endpc,D);
    }
    n= (D->strip) ? 0 : f->sizeupvalues;
    dump_int(n,D);
    for (i=0; i<n; i++) {
        dump_string(f->upvalues[i],D);
    }
}


static void dump_function(const Proto* f, const TString* p, DumpState* D) {
    dump_string((f->source==p || D->strip) ? NULL : f->source,D);
    dump_int(f->linedefined,D);
    dump_int(f->lastlinedefined,D);

    DumpChar(f->nups,D);
    DumpChar(f->numparams,D);
    DumpChar(f->is_vararg,D);
    DumpChar(f->maxstacksize,D);

    dump_code_vector(f->code, f->sizecode, D);
    dump_constants(f,D);
    dump_debug(f,D);
}

static void DumpHeader(DumpState* D)
{
 char h[LUAC_HEADERSIZE];
 luaU_header(h);
 DumpBlock(h,LUAC_HEADERSIZE,D);
}

static int dump_portable_bytecode = 1;

/*
** dump Lua function as precompiled chunk
*/
int luaU_dump (lua_State* L, const Proto* f, lua_Writer w, void* data, int strip)
{
 DumpState D;
 D.L=L;
 D.writer=w;
 D.data=data;
 D.strip=strip;
 D.status=0;

 if (dump_portable_bytecode == 1) {
    char h[LUAC_HEADERSIZE];
    luaU_header_p(h);
    DumpBlock(h,LUAC_HEADERSIZE,&D);

    setup_dump_funcs(&D);
    dump_function(f,NULL,&D);
 } else {
    DumpHeader(&D);
    DumpFunction(f,NULL,&D);
 }
 return D.status;
}
