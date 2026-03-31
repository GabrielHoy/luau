// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "lua.h"
#include "lcommon.h"

/*
** Union of all collectible objects
*/
typedef union GCObject GCObject;

/*
** Common Header for all collectible objects (in macro form, to be included in other objects)
**
** Every GC-managed heap object begins with these three bytes:
**   tt      — the lua_Type tag that identifies the object kind (LUA_TSTRING, LUA_TTABLE, …)
**   marked  — GC colour bits used by the tri-colour incremental mark-and-sweep collector
**             (white-0, white-1, gray, black; see lgc.h for the bit definitions)
**   memcat  — memory category index (0–255) used to attribute allocation bytes to a named
**             budget; set on allocation and never changed for the lifetime of the object
*/
// clang-format off
#define CommonHeader \
     uint8_t tt; uint8_t marked; uint8_t memcat
// clang-format on

/*
** Common header in struct form — used when only the GC header fields need to be accessed
** without knowing the concrete object type (e.g. inside the GCObject union).
*/
typedef struct GCheader
{
    CommonHeader;
} GCheader;

/*
** Union of all Lua values
**
** Overlays every possible runtime value in a single word-sized union.  The active
** member is determined by the `tt` field of the enclosing TValue.
*/
typedef union
{
    GCObject* gc;  ///< pointer to a heap-allocated GC object (strings, tables, closures, …)
    void* p;       ///< raw pointer for light userdata
    double n;      ///< IEEE-754 double for LUA_TNUMBER
    int b;         ///< boolean (0/1) for LUA_TBOOLEAN
    float v[2];    ///< v[0], v[1] live here; v[2] lives in TValue::extra
} Value;

/*
** Tagged Values
**
** TValue is the fundamental currency of the Luau VM.  Every stack slot, table entry,
** upvalue, and constant is a TValue.  The layout is:
**   value  — the payload; which union member is live is indicated by `tt`
**   extra  — overflow storage used when LUA_EXTRA_SIZE > 0; for light-userdata this
**             holds the userdata tag; for vectors the 3rd (and optionally 4th) component
**             lives here as a float
**   tt     — lua_Type tag (LUA_TNIL, LUA_TNUMBER, LUA_TSTRING, …)
*/

typedef struct lua_TValue
{
    Value value;             ///< the actual payload — active union member selected by `tt`
    int extra[LUA_EXTRA_SIZE]; ///< overflow: vector z/w components or lightuserdata tag
    int tt;                  ///< type tag (lua_Type); determines which Value member is live
} TValue;

// Macros to test type
#define ttisnil(o) (ttype(o) == LUA_TNIL)             ///< true iff the TValue holds nil
#define ttisnumber(o) (ttype(o) == LUA_TNUMBER)       ///< true iff the TValue holds a number
#define ttisstring(o) (ttype(o) == LUA_TSTRING)       ///< true iff the TValue holds a string
#define ttistable(o) (ttype(o) == LUA_TTABLE)         ///< true iff the TValue holds a table
#define ttisfunction(o) (ttype(o) == LUA_TFUNCTION)   ///< true iff the TValue holds a function/closure
#define ttisboolean(o) (ttype(o) == LUA_TBOOLEAN)     ///< true iff the TValue holds a boolean
#define ttisuserdata(o) (ttype(o) == LUA_TUSERDATA)   ///< true iff the TValue holds full userdata
#define ttisthread(o) (ttype(o) == LUA_TTHREAD)       ///< true iff the TValue holds a coroutine thread
#define ttisbuffer(o) (ttype(o) == LUA_TBUFFER)       ///< true iff the TValue holds a buffer
#define ttislightuserdata(o) (ttype(o) == LUA_TLIGHTUSERDATA) ///< true iff the TValue holds light userdata (unmanaged pointer)
#define ttisvector(o) (ttype(o) == LUA_TVECTOR)       ///< true iff the TValue holds a vector (float2/float3/float4)
#define ttisupval(o) (ttype(o) == LUA_TUPVAL)         ///< true iff the TValue holds an upvalue (internal use only)

// Macros to access values
#define ttype(o) ((o)->tt)                                          ///< extract the lua_Type tag from a TValue
#define gcvalue(o) check_exp(iscollectable(o), (o)->value.gc)       ///< extract the GCObject* from a collectable TValue
#define pvalue(o) check_exp(ttislightuserdata(o), (o)->value.p)     ///< extract the raw void* from a light-userdata TValue
#define nvalue(o) check_exp(ttisnumber(o), (o)->value.n)            ///< extract the double from a number TValue
#define vvalue(o) check_exp(ttisvector(o), (o)->value.v)            ///< extract the float[2] base pointer from a vector TValue (z in extra)
#define tsvalue(o) check_exp(ttisstring(o), &(o)->value.gc->ts)     ///< extract the TString* from a string TValue
#define uvalue(o) check_exp(ttisuserdata(o), &(o)->value.gc->u)     ///< extract the Udata* from a userdata TValue
#define clvalue(o) check_exp(ttisfunction(o), &(o)->value.gc->cl)   ///< extract the Closure* from a function TValue
#define hvalue(o) check_exp(ttistable(o), &(o)->value.gc->h)        ///< extract the LuaTable* from a table TValue
#define bvalue(o) check_exp(ttisboolean(o), (o)->value.b)           ///< extract the int boolean (0/1) from a boolean TValue
#define thvalue(o) check_exp(ttisthread(o), &(o)->value.gc->th)     ///< extract the lua_State* from a thread TValue
#define bufvalue(o) check_exp(ttisbuffer(o), &(o)->value.gc->buf)   ///< extract the LuauBuffer* from a buffer TValue
#define upvalue(o) check_exp(ttisupval(o), &(o)->value.gc->uv)      ///< extract the UpVal* from an upvalue TValue (internal)

#define l_isfalse(o) (ttisnil(o) || (ttisboolean(o) && bvalue(o) == 0))

#define lightuserdatatag(o) check_exp(ttislightuserdata(o), (o)->extra[0])

// Internal tags used by the VM
#define LU_TAG_ITERATOR LUA_UTAG_LIMIT

/*
** for internal debug only
*/
#define checkconsistency(obj) LUAU_ASSERT(!iscollectable(obj) || (ttype(obj) == (obj)->value.gc->gch.tt))

#define checkliveness(g, obj) LUAU_ASSERT(!iscollectable(obj) || ((ttype(obj) == (obj)->value.gc->gch.tt) && !isdead(g, (obj)->value.gc)))

// Macros to set values
#define setnilvalue(obj) ((obj)->tt = LUA_TNIL)

#define setnvalue(obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.n = (x); \
        i_o->tt = LUA_TNUMBER; \
    }

#if LUA_VECTOR_SIZE == 4
#define setvvalue(obj, x, y, z, w) \
    { \
        TValue* i_o = (obj); \
        float* i_v = i_o->value.v; \
        i_v[0] = (x); \
        i_v[1] = (y); \
        i_v[2] = (z); \
        i_v[3] = (w); \
        i_o->tt = LUA_TVECTOR; \
    }
#else
#define setvvalue(obj, x, y, z, w) \
    { \
        TValue* i_o = (obj); \
        float* i_v = i_o->value.v; \
        i_v[0] = (x); \
        i_v[1] = (y); \
        i_v[2] = (z); \
        i_o->tt = LUA_TVECTOR; \
    }
#endif

#define setpvalue(obj, x, tag) \
    { \
        TValue* i_o = (obj); \
        i_o->value.p = (x); \
        i_o->extra[0] = (tag); \
        i_o->tt = LUA_TLIGHTUSERDATA; \
    }

#define setbvalue(obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.b = (x); \
        i_o->tt = LUA_TBOOLEAN; \
    }

#define setsvalue(L, obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.gc = cast_to(GCObject*, (x)); \
        i_o->tt = LUA_TSTRING; \
        checkliveness(L->global, i_o); \
    }

#define setuvalue(L, obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.gc = cast_to(GCObject*, (x)); \
        i_o->tt = LUA_TUSERDATA; \
        checkliveness(L->global, i_o); \
    }

#define setthvalue(L, obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.gc = cast_to(GCObject*, (x)); \
        i_o->tt = LUA_TTHREAD; \
        checkliveness(L->global, i_o); \
    }

#define setbufvalue(L, obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.gc = cast_to(GCObject*, (x)); \
        i_o->tt = LUA_TBUFFER; \
        checkliveness(L->global, i_o); \
    }

#define setclvalue(L, obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.gc = cast_to(GCObject*, (x)); \
        i_o->tt = LUA_TFUNCTION; \
        checkliveness(L->global, i_o); \
    }

#define sethvalue(L, obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.gc = cast_to(GCObject*, (x)); \
        i_o->tt = LUA_TTABLE; \
        checkliveness(L->global, i_o); \
    }

#define setptvalue(L, obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.gc = cast_to(GCObject*, (x)); \
        i_o->tt = LUA_TPROTO; \
        checkliveness(L->global, i_o); \
    }

#define setupvalue(L, obj, x) \
    { \
        TValue* i_o = (obj); \
        i_o->value.gc = cast_to(GCObject*, (x)); \
        i_o->tt = LUA_TUPVAL; \
        checkliveness(L->global, i_o); \
    }

#define setobj(L, obj1, obj2) \
    { \
        const TValue* o2 = (obj2); \
        TValue* o1 = (obj1); \
        *o1 = *o2; \
        checkliveness(L->global, o1); \
    }

/*
** different types of sets, according to destination
*/

// to stack
#define setobj2s setobj
// from table to same table (no barrier)
#define setobjt2t setobj
// to table (needs barrier)
#define setobj2t setobj
// to new object (no barrier)
#define setobj2n setobj

#define setttype(obj, tt) (ttype(obj) = (tt))

#define iscollectable(o) (ttype(o) >= LUA_TSTRING)

typedef TValue* StkId; // index to stack elements

/*
** String headers for string table
**
** TString objects are immutable and interned: two TStrings with the same content
** share the same heap object, so pointer equality implies string equality.
** The variable-length string data is stored in `data[]`, allocated contiguously
** right after the header — no separate heap allocation.
*/
typedef struct TString
{
    CommonHeader;
    // 1 byte padding

    int16_t atom; ///< interning atom index used for fast metamethod name lookup; -1 if not an atom

    // 2 byte padding

    TString* next; // next string in the hash table bucket

    unsigned int hash; ///< full hash of the string content, used for table lookup and interning
    unsigned int len;  ///< byte length of the string (not including the NUL terminator)

    char data[1]; // string data is allocated right after the header
} TString;


#define getstr(ts) (ts)->data
#define svalue(o) getstr(tsvalue(o))

/*
** Full userdata — an opaque heap block managed by the GC and optionally
** carrying a metatable and a tag for type-dispatch.
** The user data bytes are stored inline at `data[]`, immediately after the header.
*/
typedef struct Udata
{
    CommonHeader;

    uint8_t tag; ///< userdata type tag (0..LUA_UTAG_LIMIT-1); used to dispatch __gc and metatables per-type

    int len; ///< size in bytes of the user payload stored at data[]

    struct LuaTable* metatable; ///< optional metatable; NULL if none is set

    // userdata is allocated right after the header
    // while the alignment is only 8 here, for sizes starting at 16 bytes, 16 byte alignment is provided
    alignas(8) char data[1]; ///< inline user payload; actual allocation is len bytes
} Udata;

/*
** Buffer object — a mutable, resizable byte array exposed to Lua via the `buffer` library.
** The byte data is stored inline immediately after the header.
*/
typedef struct LuauBuffer
{
    CommonHeader;

    unsigned int len; ///< current byte length of the buffer

    alignas(8) char data[1]; ///< inline buffer contents; actual allocation is len bytes
} Buffer;

/*
** Function Prototypes
**
** Proto holds all static information about a compiled Lua function: its bytecode,
** constants, debug info, and nested function prototypes.  At runtime a Closure
** wraps a Proto together with its captured upvalues.
*/
// clang-format off
typedef struct Proto
{
    CommonHeader;

    uint8_t nups;         ///< number of upvalues captured by this function
    uint8_t numparams;    ///< number of fixed (named) parameters
    uint8_t is_vararg;    ///< non-zero if this function accepts varargs (...)
    uint8_t maxstacksize; ///< number of stack slots required by this function (register window size)
    uint8_t flags;        ///< proto-level flags (e.g. PROTO_FLAG_NATIVE_MODULE)

    TValue* k;              // constants used by the function
    Instruction* code;      // function bytecode
    struct Proto** p;       // functions defined inside the function
    const Instruction* codeentry; ///< entry point into `code`; may differ from `code` when native code is active

    void* execdata;        ///< opaque data owned by the execution backend (JIT/AOT); NULL for pure interpreter
    uintptr_t exectarget;  ///< native entry point address set by the execution backend; 0 if not compiled

    uint8_t* lineinfo;      // for each instruction, line number as a delta from baseline
    int* abslineinfo;       // baseline line info, one entry for each 1<<linegaplog2 instructions; allocated after lineinfo
    struct LocVar* locvars; // information about local variables
    TString** upvalues;     // upvalue names
    TString* source;        ///< source file name / chunk name (e.g. "@script.lua")

    TString* debugname;    ///< human-readable function name for stack traces (may be NULL)
    uint8_t* debuginsn;    // a copy of code[] array with just opcodes

    uint8_t* typeinfo;     ///< optional native type annotation array; one byte per parameter + one for the return; NULL if absent

    void* userdata;        ///< arbitrary pointer for embedder use; not touched by the VM

    GCObject* gclist;      ///< intrusive GC grey/black traversal list link

    int sizecode;          ///< number of instructions in `code`
    int sizep;             ///< number of nested Proto* entries in `p`
    int sizelocvars;       ///< number of entries in `locvars`
    int sizeupvalues;      ///< number of entries in `upvalues` (matches `nups`)
    int sizek;             ///< number of constants in `k`
    int sizelineinfo;      ///< number of bytes in `lineinfo`
    int linegaplog2;       ///< log2 of the gap between absolute line-info entries
    int linedefined;       ///< source line where this function was defined (1-based)
    int bytecodeid;        ///< sequential id assigned by the compiler; used to correlate native code
    int sizetypeinfo;      ///< number of bytes in `typeinfo`
} Proto;
// clang-format on

/*
** Local variable debug record — maps a register slot to a name and live range.
*/
typedef struct LocVar
{
    TString* varname; ///< name of the local variable as it appears in source
    int startpc;      // first point where variable is active
    int endpc;        // first point where variable is dead
    uint8_t reg;      // register slot, relative to base, where variable is stored
} LocVar;

/*
** Upvalues
**
** An UpVal bridges a Closure and a variable that it has captured from an enclosing
** scope.  While the captured variable is still live on a thread's stack the UpVal is
** *open*: `v` points directly into the stack slot and the `open` linked-list fields
** are valid.  When the variable goes out of scope (or the thread is closed) the UpVal
** is *closed*: the value is copied into `u.value` and `v` is redirected to point there.
** Use the `upisopen(up)` macro to test the state.
*/

typedef struct UpVal
{
    CommonHeader;
    uint8_t markedopen; // set if reachable from an alive thread (only valid during atomic)

    // 4 byte padding (x64)

    TValue* v; ///< pointer to the captured value: into the stack when open, into u.value when closed
    union
    {
        TValue value; ///< owned copy of the value — live only when the upvalue is closed
        struct
        {
            // global double linked list (when open)
            struct UpVal* prev;       ///< previous open upvalue in the global list (global_State::uvhead)
            struct UpVal* next;       ///< next open upvalue in the global list

            // thread linked list (when open)
            struct UpVal* threadnext; ///< next open upvalue on the same lua_State (lua_State::openupval)
        } open;
    } u;
} UpVal;

#define upisopen(up) ((up)->v != &(up)->u.value)

/*
** Closures
**
** A Closure pairs a function body (either a C function pointer or a Lua Proto) with
** its captured upvalues.  The `isC` flag selects which union branch is live.
*/

typedef struct Closure
{
    CommonHeader;

    uint8_t isC;        ///< non-zero for C closures, zero for Lua closures
    uint8_t nupvalues;  ///< total number of upvalues carried by this closure
    uint8_t stacksize;  ///< hint for initial stack frame allocation (Lua closures only)
    uint8_t preload;    ///< number of upvalues that must be pre-loaded before first call

    GCObject* gclist;       ///< intrusive GC traversal list link
    struct LuaTable* env;   ///< environment table (global table for this closure)

    union
    {
        struct
        {
            lua_CFunction f;      ///< the C function pointer
            lua_Continuation cont; ///< optional continuation called after a yielding pcall returns
            const char* debugname; ///< static string used in stack traces; may be NULL
            TValue upvals[1];     ///< inline upvalue storage (nupvalues entries)
        } c;

        struct
        {
            struct Proto* p;  ///< the compiled Lua function prototype
            TValue uprefs[1]; ///< inline upvalue references (nupvalues entries; each is an UpVal* tagged LUA_TUPVAL)
        } l;
    };
} Closure;

#define iscfunction(o) (ttype(o) == LUA_TFUNCTION && clvalue(o)->isC)
#define isLfunction(o) (ttype(o) == LUA_TFUNCTION && !clvalue(o)->isC)

/*
** Tables
*/

/*
** TKey — a table hash-part key.  Same layout as TValue but with the `next` chain
** index packed into the upper bits of the type field to save memory.
*/
typedef struct TKey
{
    ::Value value;
    int extra[LUA_EXTRA_SIZE];
    unsigned tt : 4;   ///< type tag for this key (4 bits)
    int next : 28;     // for chaining
} TKey;

/*
** LuaNode — one slot in the hash part of a table.
** Each node stores a key/value pair; collisions are resolved by the `next` chain in TKey.
*/
typedef struct LuaNode
{
    TValue val; ///< the value stored at this slot; tt == LUA_TNIL means the slot is empty
    TKey key;   ///< the key for this slot, with hash-chain next index packed in
} LuaNode;

// copy a value into a key
#define setnodekey(L, node, obj) \
    { \
        LuaNode* n_ = (node); \
        const TValue* i_o = (obj); \
        n_->key.value = i_o->value; \
        memcpy(n_->key.extra, i_o->extra, sizeof(n_->key.extra)); \
        n_->key.tt = i_o->tt; \
        checkliveness(L->global, i_o); \
    }

// copy a value from a key
#define getnodekey(L, obj, node) \
    { \
        TValue* i_o = (obj); \
        const LuaNode* n_ = (node); \
        i_o->value = n_->key.value; \
        memcpy(i_o->extra, n_->key.extra, sizeof(i_o->extra)); \
        i_o->tt = n_->key.tt; \
        checkliveness(L->global, i_o); \
    }

/*
** LuaTable — Luau's primary data structure, combining an integer-keyed array part
** and a hash part (open-addressing with chaining).  Both parts grow independently.
*/
// clang-format off
typedef struct LuaTable
{
    CommonHeader;

    uint8_t tmcache;    // 1<<p means tagmethod(p) is not present
    uint8_t readonly;   // sandboxing feature to prohibit writes to table
    uint8_t safeenv;    // environment doesn't share globals with other scripts
    uint8_t lsizenode;  // log2 of size of `node' array
    uint8_t nodemask8;  ///< (1<<lsizenode)-1, truncated to 8 bits; used for fast modulo in hash lookup

    int sizearray; ///< allocated capacity of the `array` part (number of TValue slots)
    union
    {
        int lastfree;   ///< index into `node` of the last known free slot (hash part); used during insertion
        int aboundary;  ///< negated 'boundary' of `array' array; iff aboundary < 0
    };

    struct LuaTable* metatable; ///< optional metatable; NULL if none is attached
    TValue* array;  ///< array part — integer keys 1..sizearray are stored here (0-indexed internally)
    LuaNode* node;  ///< hash part — non-integer keys and integer keys outside the array range
    GCObject* gclist; ///< intrusive GC traversal list link
} LuaTable;
// clang-format on

/*
** `module' operation for hashing (size is always a power of 2)
*/
#define lmod(s, size) (check_exp((size & (size - 1)) == 0, (cast_to(int, (s) & ((size)-1)))))

#define twoto(x) ((int)(1 << (x)))
#define sizenode(t) (twoto((t)->lsizenode))

#define luaO_nilobject (&luaO_nilobject_)

LUAI_DATA const TValue luaO_nilobject_;

#define ceillog2(x) (luaO_log2((x)-1) + 1)

LUAI_FUNC int luaO_log2(unsigned int x);
LUAI_FUNC int luaO_rawequalObj(const TValue* t1, const TValue* t2);
LUAI_FUNC int luaO_rawequalKey(const TKey* t1, const TValue* t2);
LUAI_FUNC int luaO_str2d(const char* s, double* result);
LUAI_FUNC const char* luaO_pushvfstring(lua_State* L, const char* fmt, va_list argp);
LUAI_FUNC const char* luaO_pushfstring(lua_State* L, const char* fmt, ...);
LUAI_FUNC const char* luaO_chunkid(char* buf, size_t buflen, const char* source, size_t srclen);
