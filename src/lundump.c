/*
** $Id: lundump.c,v 2.7.1.4 2008/04/04 19:51:41 roberto Exp $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#include <string.h>

#include <stdint.h>

#define lundump_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstring.h"
#include "lundump.h"
#include "lzio.h"

typedef struct {
 lua_State* L;
 ZIO* Z;
 Mbuffer* b;
 const char* name;
} LoadState;

#ifdef LUAC_TRUST_BINARIES
#define IF(c,s)
#define error(S,s)
#else
#define IF(c,s) if (c) error(S,s)

static void error(LoadState* S, const char* why)
{
 luaO_pushfstring(S->L,"%s: %s in precompiled chunk",S->name,why);
 luaD_throw(S->L,LUA_ERRSYNTAX);
}
#endif

#define LoadMem(S,b,n,size)     LoadBlock(S,b,(n)*(size))
#define LoadByte(S)             (lu_byte)LoadChar(S)
#define LoadVar(S,x)            LoadMem(S,&x,1,sizeof(x))
#define LoadVector(S,b,n,size)  LoadMem(S,b,n,size)

static void LoadBlock(LoadState* S, void* b, size_t size)
{
 size_t r=luaZ_read(S->Z,b,size);
 IF (r!=0, "unexpected end");
}

static int LoadChar(LoadState* S)
{
 char x;
 LoadVar(S,x);
 return x;
}

static int LoadInt(LoadState* S)
{
 int x;
 LoadVar(S,x);
 IF (x<0, "bad integer");
 return x;
}

static lua_Number LoadNumber(LoadState* S)
{
 lua_Number x;
 LoadVar(S,x);
 return x;
}

static TString* LoadString(LoadState* S)
{
 size_t size;
 LoadVar(S,size);
 if (size==0)
  return NULL;
 else
 {
  char* s=luaZ_openspace(S->L,S->b,size);
  LoadBlock(S,s,size);
  return luaS_newlstr(S->L,s,size-1);       /* remove trailing '\0' */
 }
}

static void LoadCode(LoadState* S, Proto* f)
{
 int n=LoadInt(S);
 f->code=luaM_newvector(S->L,n,Instruction);
 f->sizecode=n;
 LoadVector(S,f->code,n,sizeof(Instruction));
}

static Proto* LoadFunction(LoadState* S, TString* p);

static void LoadConstants(LoadState* S, Proto* f)
{
 int i,n;
 n=LoadInt(S);
 f->k=luaM_newvector(S->L,n,TValue);
 f->sizek=n;
 for (i=0; i<n; i++) setnilvalue(&f->k[i]);
 for (i=0; i<n; i++)
 {
  TValue* o=&f->k[i];
  int t=LoadChar(S);
  switch (t)
  {
   case LUA_TNIL:
    setnilvalue(o);
    break;
   case LUA_TBOOLEAN:
    setbvalue(o,LoadChar(S)!=0);
    break;
   case LUA_TNUMBER:
    setnvalue(o,LoadNumber(S));
    break;
   case LUA_TSTRING:
    setsvalue2n(S->L,o,LoadString(S));
    break;
   default:
    error(S,"bad constant");
    break;
  }
 }
 n=LoadInt(S);
 f->p=luaM_newvector(S->L,n,Proto*);
 f->sizep=n;
 for (i=0; i<n; i++) f->p[i]=NULL;
 for (i=0; i<n; i++) f->p[i]=LoadFunction(S,f->source);
}

static void LoadDebug(LoadState* S, Proto* f)
{
 int i,n;
 n=LoadInt(S);
 f->lineinfo=luaM_newvector(S->L,n,int);
 f->sizelineinfo=n;
 LoadVector(S,f->lineinfo,n,sizeof(int));
 n=LoadInt(S);
 f->locvars=luaM_newvector(S->L,n,LocVar);
 f->sizelocvars=n;
 for (i=0; i<n; i++) f->locvars[i].varname=NULL;
 for (i=0; i<n; i++)
 {
  f->locvars[i].varname=LoadString(S);
  f->locvars[i].startpc=LoadInt(S);
  f->locvars[i].endpc=LoadInt(S);
 }
 n=LoadInt(S);
 f->upvalues=luaM_newvector(S->L,n,TString*);
 f->sizeupvalues=n;
 for (i=0; i<n; i++) f->upvalues[i]=NULL;
 for (i=0; i<n; i++) f->upvalues[i]=LoadString(S);
}

static Proto* LoadFunction(LoadState* S, TString* p)
{
 Proto* f;
 if (++S->L->nCcalls > LUAI_MAXCCALLS) error(S,"code too deep");
 f=luaF_newproto(S->L);
 setptvalue2s(S->L,S->L->top,f); incr_top(S->L);
 f->source=LoadString(S); if (f->source==NULL) f->source=p;
 f->linedefined=LoadInt(S);
 f->lastlinedefined=LoadInt(S);
 f->nups=LoadByte(S);
 f->numparams=LoadByte(S);
 f->is_vararg=LoadByte(S);
 f->maxstacksize=LoadByte(S);
 LoadCode(S,f);
 LoadConstants(S,f);
 LoadDebug(S,f);
 IF (!luaG_checkcode(f), "bad code");
 S->L->top--;
 S->L->nCcalls--;
 return f;
}

static int check_header(LoadState* S) {
    char h[LUAC_HEADERSIZE];
    char s[LUAC_HEADERSIZE];

    LoadBlock(S,s,LUAC_HEADERSIZE);

    luaU_header_p(h);
    if (memcmp(h, s, LUAC_HEADERSIZE) == 0) {
        return 1;
    } else {
        luaU_header(h);
        IF (memcmp(h,s,LUAC_HEADERSIZE)!=0, "bad header");
        return 0;
    }
}

/*
* make header
*/
void luaU_header (char* h)
{
 int x=1;
 memcpy(h,LUA_SIGNATURE,sizeof(LUA_SIGNATURE)-1);
 h+=sizeof(LUA_SIGNATURE)-1;
 *h++=(char)LUAC_VERSION;
 *h++=(char)LUAC_FORMAT;
 *h++=(char)*(char*)&x;             /* endianness */
 *h++=(char)sizeof(int);
 *h++=(char)sizeof(size_t);
 *h++=(char)sizeof(Instruction);
 *h++=(char)sizeof(lua_Number);
 *h++=(char)(((lua_Number)0.5)==0);     /* is lua_Number integral? */
}

#define MY_LUAC_FORMAT 0x66

void luaU_header_p (char* h) {
    memcpy(h,LUA_SIGNATURE,sizeof(LUA_SIGNATURE)-1);
    h   += sizeof(LUA_SIGNATURE)-1;
    *h++ =(char)LUAC_VERSION;
    *h++ =(char)MY_LUAC_FORMAT;

    *h++ = (char)1; // endianness
    *h++ = (char)4; // sizeof int
    *h++ = (char)8; // sizeof size_t
    *h++ = (char)4; // sizeof instruction
    *h++ = (char)8; // sizeof number

    *h++ = (char)(((lua_Number)0.5)==0);
}

static int (*load_int)(LoadState* S) = NULL;
static size_t (*load_size_t)(LoadState* S) = NULL;
static uint32_t*  (*load_byte4_vector)(LoadState* S, int * pn) = NULL;
static lua_Number (*load_number)(LoadState* S) = NULL;

static int load_int_o(LoadState* S) {
    int32_t x;
    LoadBlock(S, &x, 4);
    IF (x<0, "bad integer");
    return (int)x;
}

static int load_int_s(LoadState* S) {
    int32_t x;
    LoadBlock(S,&x,4);
    x = BSWAP_32(x);
    IF (x<0, "bad integer");
    return (int)x;
}

static size_t load_size_t_o(LoadState* S) {
    uint64_t x;
    LoadBlock(S, &x, 8);
    return (size_t)x;
}

static size_t load_size_t_s(LoadState* S) {
    uint64_t x;
    LoadBlock(S, &x, 8);
    x = BSWAP_64(x);
    return (size_t)x;
}

static TString* load_string(LoadState* S) {
    uint64_t size = load_size_t(S);
    if (size==0) {
        return NULL;
    } else {
        char* s=luaZ_openspace(S->L,S->b,size);
        LoadBlock(S,s,size);
        return luaS_newlstr(S->L,s,size-1);
    }
}

static lua_Number load_number_o(LoadState* S) {
    double d;
    LoadBlock(S, &d, 8);
    return (lua_Number)d;
}

static lua_Number load_number_s(LoadState* S) {
    union {
        double d;
        uint64_t u;
    } du;
    LoadBlock(S, &du.u, 8);
    du.u = BSWAP_64(du.u);
    return (lua_Number)du.d;
}

static uint32_t * load_byte4_vector_o(LoadState* S, int * pn) {
    *pn = load_int(S);
    uint32_t * ds = luaM_newvector(S->L,*pn,uint32_t);
    LoadVector(S,ds,*pn,sizeof(uint32_t));
    return ds;
}

static uint32_t * load_byte4_vector_s(LoadState* S, int *pn) {
    *pn = load_int(S);
    uint32_t * ds = luaM_newvector(S->L,*pn,uint32_t);
    LoadVector(S,ds,*pn,sizeof(uint32_t));
    for (int i = 0; i < *pn; i++) {
        ds[i] = BSWAP_32(ds[i]);
    }
    return ds;
}

static void setup_load_funcs(LoadState* S) {
    if (load_int == NULL) {
        int x = 0x1;
        if (*(int8_t *)&x) {
            load_int          = load_int_o;
            load_size_t       = load_size_t_o;
            load_byte4_vector = load_byte4_vector_o;
        } else {
            load_int          = load_int_s;
            load_size_t       = load_size_t_s;
            load_byte4_vector = load_byte4_vector_s;
        }
    }
    if (load_number == NULL) {
        double d = 1.2344999991522893623141499119810760021209716796875;
        if (memcmp(&d, "\x78\x56\x34\x12\x83\xC0\xF3\x3F", 8) == 0) {
            load_number = load_number_o;
        } else if (memcmp(&d, "\x3F\xF3\xC0\x83\x12\x34\x56\x78", 8) == 0) {
            load_number = load_number_s;
        } else {
            luaO_pushfstring(S->L,"load: unknown number format");
            luaD_throw(S->L, LUA_ERRERR);
        }
    }
}



static void load_code(LoadState* S, Proto* f) {
    int n;
    uint32_t * v = load_byte4_vector(S, &n);

    if (sizeof(Instruction) == sizeof(uint32_t)) {
        f->sizecode = n;
        f->code     = (Instruction *)v;
    } else {
        f->sizecode = n;
        f->code     = luaM_newvector(S->L,n,Instruction);
        Instruction * pi = f->code;
        for (int i = 0; i < n; i++) {
            *pi++ = (Instruction)v[i];
        }
        luaM_freearray(S->L, v, n, uint32_t);
    }
}

static Proto* load_function(LoadState* S, TString* p);

static void load_constants(LoadState* S, Proto* f) {
    int i,n;
    n=load_int(S);
    f->k=luaM_newvector(S->L,n,TValue);
    f->sizek=n;
    for (i=0; i<n; i++) setnilvalue(&f->k[i]);
    for (i=0; i<n; i++) {
        TValue* o=&f->k[i];
        int t=LoadChar(S);
        switch (t) {
            case LUA_TNIL:
                setnilvalue(o);
                break;
            case LUA_TBOOLEAN:
                setbvalue(o,LoadChar(S)!=0);
                break;
            case LUA_TNUMBER:
                setnvalue(o,load_number(S));
                break;
            case LUA_TSTRING:
                setsvalue2n(S->L,o,load_string(S));
                break;
            default:
                error(S,"bad constant");
                break;
        }
    }
    n=load_int(S);
    f->p=luaM_newvector(S->L,n,Proto*);
    f->sizep=n;
    for (i=0; i<n; i++) f->p[i]=NULL;
    for (i=0; i<n; i++) f->p[i]=load_function(S,f->source);
}

static void load_debug(LoadState* S, Proto* f) {
    int i,n;

    int32_t * v = (int32_t*)load_byte4_vector(S, &n);

    f->sizelineinfo = n;
    if (sizeof(int) == sizeof(int32_t) ) {
        f->lineinfo = (int*)v;
    } else {
        f->lineinfo = luaM_newvector(S->L,n,int);
        int *pi = f->lineinfo;
        for (int i = 0; i<n; i++) {
            *pi++ = (int)v[i];
        }
        luaM_freearray(S->L,v,n,int32_t);
    }

    n = load_int(S);
    f->locvars=luaM_newvector(S->L,n,LocVar);
    f->sizelocvars=n;

    for (i=0; i<n; i++) f->locvars[i].varname=NULL;
    for (i=0; i<n; i++) {
        f->locvars[i].varname=load_string(S);
        f->locvars[i].startpc=load_int(S);
        f->locvars[i].endpc=load_int(S);
    }
    n=load_int(S);
    f->upvalues=luaM_newvector(S->L,n,TString*);
    f->sizeupvalues=n;
    for (i=0; i<n; i++) f->upvalues[i]=NULL;
    for (i=0; i<n; i++) f->upvalues[i]=load_string(S);
}

static Proto* load_function(LoadState* S, TString* p) {
    Proto* f;
    if (++S->L->nCcalls > LUAI_MAXCCALLS) error(S,"code too deep");

    f=luaF_newproto(S->L);
    setptvalue2s(S->L,S->L->top,f); incr_top(S->L);

    f->source=LoadString(S); if (f->source==NULL) f->source=p;

    f->linedefined     = load_int(S);
    f->lastlinedefined = load_int(S);
    f->nups            = LoadByte(S);
    f->numparams       = LoadByte(S);
    f->is_vararg       = LoadByte(S);
    f->maxstacksize    = LoadByte(S);

    load_code(S,f);
    load_constants(S,f);
    load_debug(S,f);

    IF (!luaG_checkcode(f), "bad code");

    S->L->top--;
    S->L->nCcalls--;
    return f;
}


/*
** load precompiled chunk
*/
Proto* luaU_undump (lua_State* L, ZIO* Z, Mbuffer* buff, const char* name) {
    LoadState S;

    if (*name=='@' || *name=='=')
        S.name = name+1;
    else if (*name==LUA_SIGNATURE[0])
        S.name = "binary string";
    else
        S.name = name;
    S.L = L;
    S.Z = Z;
    S.b = buff;

    if (check_header(&S)) {
        setup_load_funcs(&S);
        return load_function(&S,luaS_newliteral(L,"=?"));
    } else {
        return LoadFunction(&S,luaS_newliteral(L,"=?"));
    }

}

