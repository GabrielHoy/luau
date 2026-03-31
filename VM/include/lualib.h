// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "lua.h"

/** @brief Raises a formatted error.  Macro wrapper for luaL_errorL() that accepts `__VA_ARGS__`. */
#define luaL_error(L, fmt, ...) luaL_errorL(L, fmt, ##__VA_ARGS__)
/** @brief Raises a "bad argument #N" type-error.  Macro wrapper for luaL_typeerrorL(). */
#define luaL_typeerror(L, narg, tname) luaL_typeerrorL(L, narg, tname)
/** @brief Raises a "bad argument #N" error with custom message.  Macro wrapper for luaL_argerrorL(). */
#define luaL_argerror(L, narg, extramsg) luaL_argerrorL(L, narg, extramsg)

/** @brief Name/function pair used to register a library of C functions.
 *
 *  Pass an array terminated by `{NULL, NULL}` to luaL_register(). */
struct luaL_Reg
{
    const char* name;   ///< Function name (NUL-terminated), or NULL to mark end-of-array.
    lua_CFunction func; ///< C function implementing this library entry.
};
typedef struct luaL_Reg luaL_Reg;

/** @brief Registers a list of C functions into a global (or existing) table.
 *
 *  `[-0, +1, m]`
 *
 *  If @p libname is non-NULL, creates (or reuses) a global table of that name and registers
 *  the functions into it, leaving the table on the stack.  If @p libname is NULL, registers
 *  the functions into the table at the top of the stack.
 *
 *  @param L       The Lua state.
 *  @param libname Name of the global library table, or NULL to use the stack top.
 *  @param l       Zero-terminated array of luaL_Reg name/function pairs. */
LUALIB_API void luaL_register(lua_State* L, const char* libname, const luaL_Reg* l);

/** @brief Pushes the value of field @p e from the metatable of the object at @p obj.
 *
 *  `[-0, +1|0, e]`
 *
 *  @param L    The Lua state.
 *  @param obj  Stack index of the object.
 *  @param e    Metafield name (NUL-terminated).
 *  @return     Non-zero and pushes the field if it exists, otherwise returns 0 and pushes nothing. */
LUALIB_API int luaL_getmetafield(lua_State* L, int obj, const char* e);

/** @brief Invokes the metamethod @p e on the object at @p obj, pushing its result.
 *
 *  `[-0, +1|0, e]`
 *
 *  @param L    The Lua state.
 *  @param obj  Stack index of the object.
 *  @param e    Metamethod name (NUL-terminated).
 *  @return     Non-zero if the metamethod was found and called, 0 otherwise. */
LUALIB_API int luaL_callmeta(lua_State* L, int obj, const char* e);

/** @brief Raises "bad argument #N to 'fn' (tname expected, got T)" — never returns.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param tname Expected type name (e.g. `"string"`, `"MyObject"`). */
LUALIB_API l_noret luaL_typeerrorL(lua_State* L, int narg, const char* tname);

/** @brief Raises "bad argument #N to 'fn' (extramsg)" — never returns.
 *
 *  `[-0, +0, e]`
 *
 *  @param L         The Lua state.
 *  @param narg      1-based argument index.
 *  @param extramsg  Detail appended in the error message. */
LUALIB_API l_noret luaL_argerrorL(lua_State* L, int narg, const char* extramsg);

/** @brief Checks that argument @p numArg is a string and returns it with its length.
 *
 *  `[-0, +0, e]`
 *
 *  Raises a type error if the argument is not a string (numbers are NOT coerced, unlike lua_tolstring).
 *
 *  @param L       The Lua state.
 *  @param numArg  1-based argument index.
 *  @param l       If non-NULL, receives the string length in bytes.
 *  @return        Pointer to the string data. */
LUALIB_API const char* luaL_checklstring(lua_State* L, int numArg, size_t* l);

/** @brief Like luaL_checklstring() but returns @p def if the argument is absent or nil.
 *
 *  `[-0, +0, e]`
 *
 *  @param L       The Lua state.
 *  @param numArg  1-based argument index.
 *  @param def     Default string returned when the argument is absent/nil.
 *  @param l       If non-NULL, receives the length of the returned string.
 *  @return        The string value or @p def. */
LUALIB_API const char* luaL_optlstring(lua_State* L, int numArg, const char* def, size_t* l);

/** @brief Checks that argument @p numArg is a number and returns it as a `double`.
 *
 *  `[-0, +0, e]`
 *
 *  @param L       The Lua state.
 *  @param numArg  1-based argument index.
 *  @return        The number value. */
LUALIB_API double luaL_checknumber(lua_State* L, int numArg);

/** @brief Like luaL_checknumber() but returns @p def if the argument is absent or nil.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param nArg  1-based argument index.
 *  @param def   Default value.
 *  @return      The number value or @p def. */
LUALIB_API double luaL_optnumber(lua_State* L, int nArg, double def);

/** @brief Checks that argument @p narg is a boolean and returns it as an int (0 or 1).
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @return      0 for `false`, 1 for `true`. */
LUALIB_API int luaL_checkboolean(lua_State* L, int narg);

/** @brief Like luaL_checkboolean() but returns @p def if the argument is absent or nil.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default value (0 or 1).
 *  @return      The boolean value or @p def. */
LUALIB_API int luaL_optboolean(lua_State* L, int narg, int def);

/** @brief Checks that argument @p numArg is an integer-valued number and returns it.
 *
 *  `[-0, +0, e]`
 *
 *  Raises an error if the value is not an integer or cannot be represented as `int`.
 *
 *  @param L       The Lua state.
 *  @param numArg  1-based argument index.
 *  @return        The integer value. */
LUALIB_API int luaL_checkinteger(lua_State* L, int numArg);

/** @brief Like luaL_checkinteger() but returns @p def if the argument is absent or nil.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param nArg  1-based argument index.
 *  @param def   Default value.
 *  @return      The integer value or @p def. */
LUALIB_API int luaL_optinteger(lua_State* L, int nArg, int def);

/** @brief Checks that argument @p numArg is an unsigned integer and returns it.
 *
 *  `[-0, +0, e]`
 *
 *  @param L       The Lua state.
 *  @param numArg  1-based argument index.
 *  @return        The unsigned integer value. */
LUALIB_API unsigned luaL_checkunsigned(lua_State* L, int numArg);

/** @brief Like luaL_checkunsigned() but returns @p def if the argument is absent or nil.
 *
 *  `[-0, +0, e]`
 *
 *  @param L       The Lua state.
 *  @param numArg  1-based argument index.
 *  @param def     Default value.
 *  @return        The unsigned value or @p def. */
LUALIB_API unsigned luaL_optunsigned(lua_State* L, int numArg, unsigned def);

/** @brief Checks that argument @p narg is a vector and returns a pointer to its components.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @return      Pointer to float[LUA_VECTOR_SIZE] components. */
LUALIB_API const float* luaL_checkvector(lua_State* L, int narg);

/** @brief Like luaL_checkvector() but returns @p def if the argument is absent or nil.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default vector (pointer to float[LUA_VECTOR_SIZE]), or NULL.
 *  @return      The vector pointer or @p def. */
LUALIB_API const float* luaL_optvector(lua_State* L, int narg, const float* def);

/** @brief Ensures the stack has room for @p sz extra slots, raising an error if not possible.
 *
 *  `[-0, +0, e]`
 *
 *  Unlike lua_checkstack(), this raises a Lua error rather than returning 0.
 *
 *  @param L    The Lua state.
 *  @param sz   Number of additional stack slots needed.
 *  @param msg  Message appended to the error, or NULL. */
LUALIB_API void luaL_checkstack(lua_State* L, int sz, const char* msg);

/** @brief Checks that argument @p narg has type @p t, raising a type error otherwise.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param t     Expected lua_Type value (e.g. LUA_TTABLE). */
LUALIB_API void luaL_checktype(lua_State* L, int narg, int t);

/** @brief Checks that argument @p narg is present (not LUA_TNONE), raising an error otherwise.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index. */
LUALIB_API void luaL_checkany(lua_State* L, int narg);

/** @brief Creates (or retrieves) a metatable named @p tname in the registry.
 *
 *  `[-0, +1, m]`
 *
 *  If the registry already has an entry for @p tname the existing table is pushed.
 *  The table can later be retrieved with `luaL_getmetatable(L, tname)`.
 *
 *  @param L      The Lua state.
 *  @param tname  Unique name for the metatable.
 *  @return       Non-zero if the metatable was freshly created, 0 if it already existed. */
LUALIB_API int luaL_newmetatable(lua_State* L, const char* tname);

/** @brief Checks that argument @p ud is a full userdata whose metatable matches @p tname.
 *
 *  `[-0, +0, e]`
 *
 *  Raises an error if the argument is not the expected userdata type.
 *
 *  @param L      The Lua state.
 *  @param ud     1-based argument index.
 *  @param tname  Expected metatable name (previously registered with luaL_newmetatable).
 *  @return       Pointer to the userdata memory block. */
LUALIB_API void* luaL_checkudata(lua_State* L, int ud, const char* tname);

/** @brief Checks that argument @p narg is a buffer and returns a pointer to its data.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param len   If non-NULL, receives the buffer size in bytes.
 *  @return      Pointer to the raw buffer data. */
LUALIB_API void* luaL_checkbuffer(lua_State* L, int narg, size_t* len);

/** @brief Pushes a location string `"chunkname:currentline: "` onto the stack.
 *
 *  `[-0, +1, m]`
 *
 *  Used internally by luaL_errorL() to prefix error messages.
 *
 *  @param L    The Lua state.
 *  @param lvl  Call-stack level to blame (0 = C caller, 1 = its Lua caller, etc.). */
LUALIB_API void luaL_where(lua_State* L, int lvl);

/** @brief Formats an error message and raises it — never returns.
 *
 *  `[-0, +0, e]`
 *
 *  Prefixes the message with location info from the calling Lua function.
 *  Use the `luaL_error(L, fmt, ...)` macro instead for cleaner callsites.
 *
 *  @param L    The Lua state.
 *  @param fmt  printf-style format string (Lua subset: %d, %s, %f, %p, %q, %%). */
LUALIB_API LUA_PRINTF_ATTR(2, 3) l_noret luaL_errorL(lua_State* L, const char* fmt, ...);

/** @brief Checks that argument @p narg is a string that matches one of the strings in @p lst.
 *
 *  `[-0, +0, e]`
 *
 *  Returns the index (0-based) of the matching string in @p lst.
 *  If the argument is absent or nil, @p def is matched against @p lst instead.
 *  Raises an error if there is no match.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default string used when the argument is absent/nil (may be NULL).
 *  @param lst   NULL-terminated array of acceptable string values.
 *  @return      0-based index of the matched string in @p lst. */
LUALIB_API int luaL_checkoption(lua_State* L, int narg, const char* def, const char* const lst[]);

/** @brief Converts the value at @p idx to a string and pushes the result.
 *
 *  `[-0, +1, m]`
 *
 *  Calls the `__tostring` metamethod if present; otherwise uses lua_typename() for non-string types.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the value to convert.
 *  @param len  If non-NULL, receives the string length.
 *  @return     Pointer to the resulting string on the stack. */
LUALIB_API const char* luaL_tolstring(lua_State* L, int idx, size_t* len);

/** @brief Creates a new Lua state with a default allocator (using system malloc/realloc/free).
 *
 *  `[-0, +0, -]`
 *
 *  Convenience wrapper around lua_newstate() for embedders that do not need a custom allocator.
 *
 *  @return  Pointer to the new state, or NULL on allocation failure. */
LUALIB_API lua_State* luaL_newstate(void);

/** @brief Navigates a dotted table path @p fname starting from the table at @p idx.
 *
 *  `[-0, +1, m]`
 *
 *  Creates intermediate tables as needed.  For example, `luaL_findtable(L, idx, "a.b.c", 0)`
 *  traverses (or creates) `t.a`, `t.a.b`, and pushes `t.a.b.c`.
 *
 *  @param L       The Lua state.
 *  @param idx     Stack index of the root table.
 *  @param fname   Dotted field path (e.g. `"package.loaded"`).
 *  @param szhint  Size hint for newly created tables (0 is fine if unknown).
 *  @return        NULL on success (table pushed), or a pointer into @p fname indicating where
 *                 traversal failed because a non-table value was encountered. */
LUALIB_API const char* luaL_findtable(lua_State* L, int idx, const char* fname, int szhint);

/** @brief Returns the type name of the value at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  Convenience wrapper around `lua_typename(L, lua_type(L, idx))`.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     Type name string (e.g. `"table"`, `"nil"`, `"number"`). */
LUALIB_API const char* luaL_typename(lua_State* L, int idx);

/** @brief Performs a Lua call that is safe to use inside a yieldable C function.
 *
 *  `[-(nargs+1), +nresults, e]`
 *
 *  When the called function yields, control returns correctly to the host coroutine scheduler
 *  rather than crashing with a "attempt to yield across C-call boundary" error.
 *  Use this instead of lua_call() when the calling C function may be resumed after a yield.
 *
 *  @param L        The Lua state.
 *  @param nargs    Number of arguments on the stack above the function.
 *  @param nresults Number of return values expected (or LUA_MULTRET).
 *  @return         Status code from the underlying call. */
// wrapper for making calls from yieldable C functions
LUALIB_API int luaL_callyieldable(lua_State* L, int nargs, int nresults);

/** @brief Pushes a traceback string for state @p L1 onto state @p L's stack.
 *
 *  `[-0, +1, m]`
 *
 *  @param L      The Lua state to push the traceback onto.
 *  @param L1     The Lua state whose call stack is traced (may equal @p L).
 *  @param msg    Optional message prepended to the traceback (may be NULL).
 *  @param level  Call-stack level to start from (1 = the function that called traceback). */
LUALIB_API void luaL_traceback(lua_State* L, lua_State* L1, const char* msg, int level);

/*
** ===============================================================
** some useful macros
** ===============================================================
*/

/** @brief Raises @p extramsg as an argument error if @p cond is false.  No-op when @p cond is true. */
#define luaL_argcheck(L, cond, arg, extramsg) ((void)((cond) ? (void)0 : luaL_argerror(L, arg, extramsg)))
/** @brief Raises a type error for @p arg if @p cond is false.  No-op when @p cond is true. */
#define luaL_argexpected(L, cond, arg, tname) ((void)((cond) ? (void)0 : luaL_typeerror(L, arg, tname)))

/** @brief Checks that argument @p n is a string and returns it (no length).  Raises on failure. */
#define luaL_checkstring(L, n) (luaL_checklstring(L, (n), NULL))
/** @brief Like luaL_checkstring() but returns @p d when the argument is absent or nil. */
#define luaL_optstring(L, n, d) (luaL_optlstring(L, (n), (d), NULL))

/** @brief Pushes the metatable named @p n from the registry.  `[-0, +1, -]` */
#define luaL_getmetatable(L, n) (lua_getfield(L, LUA_REGISTRYINDEX, (n)))

/** @brief Applies optional-argument helper: if index @p n is absent or nil, returns @p d; otherwise calls `f(L, n)`. */
#define luaL_opt(L, f, n, d) (lua_isnoneornil(L, (n)) ? (d) : f(L, (n)))

// generic buffer manipulation

/** @brief Dynamic string buffer for building Lua strings piece by piece.
 *
 *  Initialise with luaL_buffinit() or luaL_buffinitsize().  Append data with
 *  luaL_addlstring(), luaL_addvalue(), luaL_addvalueany(), or the luaL_addchar() macro.
 *  Finalise with luaL_pushresult() or luaL_pushresultsize().
 *
 *  @note When the internal @p buffer is exhausted a mutable Lua string is placed on the
 *        stack at (top-1) and @p storage points to it.  Most functions expect this mutable
 *        string to remain at (top-1); the exception is luaL_addvalue() which expects the
 *        value-to-add at (top) and the buffer at (top-2). */
struct luaL_Strbuf
{
    char* p;   ///< Write cursor: next byte will be written here.
    char* end; ///< One past the end of the current usable buffer region.
    lua_State* L;            ///< Owning Lua state.
    struct TString* storage; ///< Internal mutable string used when the inline buffer overflows (may be NULL).
    char buffer[LUA_BUFFERSIZE]; ///< Inline storage; used first before spilling to @p storage.
};
typedef struct luaL_Strbuf luaL_Strbuf;

// compatibility typedef: this type is called luaL_Buffer in Lua headers
// renamed to luaL_Strbuf to reduce confusion with internal VM buffer type
/** @brief Alias for luaL_Strbuf — matches the `luaL_Buffer` name used in standard Lua 5.x headers. */
typedef struct luaL_Strbuf luaL_Buffer;

// when internal buffer storage is exhausted, a mutable string value 'storage' will be placed on the stack
// in general, functions expect the mutable string buffer to be placed on top of the stack (top-1)
// with the exception of luaL_addvalue that expects the value at the top and string buffer further away (top-2)

/** @brief Appends a single character @p c to buffer @p B, flushing if necessary. */
#define luaL_addchar(B, c) ((void)((B)->p < (B)->end || luaL_prepbuffsize(B, 1)), (*(B)->p++ = (char)(c)))
/** @brief Appends a NUL-terminated string @p s to buffer @p B. */
#define luaL_addstring(B, s) luaL_addlstring(B, s, strlen(s))

/** @brief Initialises buffer @p B for use with Lua state @p L.
 *
 *  `[-0, +0, -]`
 *
 *  Must be called before using any other luaL_Strbuf functions.
 *
 *  @param L  The Lua state.
 *  @param B  Uninitialized buffer struct to initialise. */
LUALIB_API void luaL_buffinit(lua_State* L, luaL_Strbuf* B);

/** @brief Initialises @p B and pre-allocates at least @p size bytes, returning a write pointer.
 *
 *  `[-0, +1, m]`
 *
 *  Useful when the final string size is known in advance.  The returned pointer points to a
 *  writable region of at least @p size bytes; after writing, call luaL_pushresultsize().
 *
 *  @param L     The Lua state.
 *  @param B     Buffer struct to initialise.
 *  @param size  Minimum number of bytes to pre-allocate.
 *  @return      Pointer to the writable region. */
LUALIB_API char* luaL_buffinitsize(lua_State* L, luaL_Strbuf* B, size_t size);

/** @brief Ensures @p B has at least @p size contiguous bytes available and returns a write pointer.
 *
 *  `[-0, +0|1, m]`
 *
 *  May push a mutable string onto the stack if the internal buffer must grow.
 *
 *  @param B     The buffer.
 *  @param size  Required number of contiguous bytes.
 *  @return      Pointer to at least @p size writable bytes. */
LUALIB_API char* luaL_prepbuffsize(luaL_Buffer* B, size_t size);

/** @brief Appends @p l bytes of data at @p s to buffer @p B.
 *
 *  `[-0, +0|1, m]`
 *
 *  @param B  The buffer.
 *  @param s  Data to append.
 *  @param l  Number of bytes to append. */
LUALIB_API void luaL_addlstring(luaL_Strbuf* B, const char* s, size_t l);

/** @brief Pops the value at the top of the stack and appends its string representation to @p B.
 *
 *  `[-1, +0|1, m]`
 *
 *  The buffer's mutable string (if any) must be at stack position (top-2) when this is called,
 *  because the value-to-add must be at (top-1).
 *
 *  @param B  The buffer. */
LUALIB_API void luaL_addvalue(luaL_Strbuf* B);

/** @brief Appends the string representation of the value at @p idx in @p B's state.
 *
 *  `[-0, +0|1, m]`
 *
 *  Unlike luaL_addvalue(), this does NOT pop the value from the stack.
 *
 *  @param B    The buffer.
 *  @param idx  Stack index of the value to append. */
LUALIB_API void luaL_addvalueany(luaL_Strbuf* B, int idx);

/** @brief Finalises the buffer and pushes the accumulated string as a Lua string.
 *
 *  `[-0, +1, m]`
 *
 *  The buffer must not be used after this call.
 *
 *  @param B  The buffer to finalise. */
LUALIB_API void luaL_pushresult(luaL_Strbuf* B);

/** @brief Like luaL_pushresult() but uses only the first @p size bytes written via luaL_buffinitsize().
 *
 *  `[-0, +1, m]`
 *
 *  Use this when you wrote exactly @p size bytes into the pointer returned by luaL_buffinitsize().
 *
 *  @param B     The buffer to finalise.
 *  @param size  Number of bytes of the pre-allocated region that were actually written. */
LUALIB_API void luaL_pushresultsize(luaL_Strbuf* B, size_t size);

// builtin libraries
/** @brief Opens the base library (`print`, `pairs`, `ipairs`, `error`, `pcall`, `type`, etc.).
 *
 *  `[-0, +0, m]`
 *
 *  @param L  The Lua state.
 *  @return   Always 0 (no values pushed). */
LUALIB_API int luaopen_base(lua_State* L);

/** @brief Library name string for the coroutine library. */
#define LUA_COLIBNAME "coroutine"
/** @brief Opens the coroutine library (`coroutine.create`, `.resume`, `.yield`, etc.).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_coroutine(lua_State* L);

/** @brief Library name string for the table library. */
#define LUA_TABLIBNAME "table"
/** @brief Opens the table library (`table.insert`, `.remove`, `.sort`, `.move`, etc.).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_table(lua_State* L);

/** @brief Library name string for the os library. */
#define LUA_OSLIBNAME "os"
/** @brief Opens the os library (`os.time`, `.clock`, `.date`, `.difftime`).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_os(lua_State* L);

/** @brief Library name string for the string library. */
#define LUA_STRLIBNAME "string"
/** @brief Opens the string library (`string.format`, `.sub`, `.find`, `.gmatch`, etc.).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_string(lua_State* L);

/** @brief Library name string for the bit32 library. */
#define LUA_BITLIBNAME "bit32"
/** @brief Opens the bit32 library (`bit32.band`, `.bor`, `.bxor`, `.lshift`, etc.).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_bit32(lua_State* L);

/** @brief Library name string for the buffer library. */
#define LUA_BUFFERLIBNAME "buffer"
/** @brief Opens the buffer library (`buffer.create`, `.readi8`, `.writeu32`, etc.).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_buffer(lua_State* L);

/** @brief Library name string for the utf8 library. */
#define LUA_UTF8LIBNAME "utf8"
/** @brief Opens the utf8 library (`utf8.char`, `.codepoint`, `.codes`, `.len`, etc.).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_utf8(lua_State* L);

/** @brief Library name string for the math library. */
#define LUA_MATHLIBNAME "math"
/** @brief Opens the math library (`math.sin`, `.floor`, `.huge`, `.pi`, etc.).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_math(lua_State* L);

/** @brief Library name string for the debug library. */
#define LUA_DBLIBNAME "debug"
/** @brief Opens the debug library (`debug.traceback`, `.info`, `.getupvalue`, etc.).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_debug(lua_State* L);

/** @brief Library name string for the vector library. */
#define LUA_VECLIBNAME "vector"
/** @brief Opens the vector library (vector construction and component access).
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @return   1 (pushes the library table). */
LUALIB_API int luaopen_vector(lua_State* L);

/** @brief Opens all standard built-in libraries into the given state.
 *
 *  `[-0, +0, m]`
 *
 *  Calls each luaopen_* function and registers the results as globals.
 *  Equivalent to calling luaopen_base, luaopen_coroutine, luaopen_table,
 *  luaopen_os, luaopen_string, luaopen_bit32, luaopen_buffer, luaopen_utf8,
 *  luaopen_math, luaopen_debug, and luaopen_vector.
 *
 *  @param L  The Lua state. */
// open all builtin libraries
LUALIB_API void luaL_openlibs(lua_State* L);

/** @brief Marks all standard library tables as read-only and freezes the global environment.
 *
 *  `[-0, +0, -]`
 *
 *  After this call, Lua code cannot modify built-in library tables.
 *  Typically called after luaL_openlibs() when setting up a sandboxed execution environment.
 *  Must be called before luaL_sandboxthread().
 *
 *  @param L  The Lua state. */
// sandbox libraries and globals
LUALIB_API void luaL_sandbox(lua_State* L);

/** @brief Replaces the current thread's global environment with a fresh writable table.
 *
 *  `[-0, +0, m]`
 *
 *  The new environment inherits from the (now read-only) global table via `__index`, so
 *  standard library functions remain accessible but writes go into the per-thread table.
 *  Call luaL_sandbox() on the main state first, then luaL_sandboxthread() on each coroutine
 *  that should run untrusted code.
 *
 *  @param L  The coroutine thread to sandbox. */
LUALIB_API void luaL_sandboxthread(lua_State* L);
