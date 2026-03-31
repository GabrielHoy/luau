// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "luaconf.h"



/** @brief Sentinel value for multiple results in `lua_pcall` and `lua_call`,
 * ..." `nresults` meaning 'return all results' ".
 */
#define LUA_MULTRET (-1)

/*
** pseudo-indices
*/
/** @brief Pseudo-index of the Lua registry — a global table accessible only from C. */
#define LUA_REGISTRYINDEX (-LUAI_MAXCSTACK - 2000)
/** @brief Pseudo-index of the current C function's environment table (Luau: unused, kept for compat). */
#define LUA_ENVIRONINDEX  (-LUAI_MAXCSTACK - 2001)
/** @brief Pseudo-index of the global environment table (`_G`). */
#define LUA_GLOBALSINDEX  (-LUAI_MAXCSTACK - 2002)
/** @brief Returns the pseudo-index for upvalue number @p i of the running C closure. */
#define lua_upvalueindex(i) (LUA_GLOBALSINDEX - (i))
/** @brief Returns non-zero when @p i is a pseudo-index (registry, environ, globals, or upvalue). */
#define lua_ispseudo(i) ((i) <= LUA_REGISTRYINDEX)

/** @brief Return/status codes from lua_pcall, lua_resume, lua_status, etc. */
// thread status; 0 is OK
enum lua_Status
{
    LUA_OK = 0,       ///< Success / no error.
    LUA_YIELD,        ///< Coroutine has yielded.
    LUA_ERRRUN,       ///< Runtime error.
    LUA_ERRSYNTAX,    ///< Legacy error code, preserved for compatibility (Luau uses bytecode, not source).
    LUA_ERRMEM,       ///< Memory allocation failure.
    LUA_ERRERR,       ///< Error while running the error handler.
    LUA_BREAK,        ///< Thread yielded for a debug breakpoint.
};

/** @brief Coroutine status codes returned by lua_costatus(). */
enum lua_CoStatus
{
    LUA_CORUN = 0, ///< Coroutine is currently running.
    LUA_COSUS,     ///< Coroutine is suspended (yielded or not yet started).
    LUA_CONOR,     ///< Coroutine is "normal" — it resumed another coroutine and is waiting.
    LUA_COFIN,     ///< Coroutine has finished execution normally.
    LUA_COERR,     ///< Coroutine finished with an unhandled error.
};

typedef struct lua_State lua_State;

/** @brief Type for C functions callable from Lua.  Must return the number of results left on the stack. */
typedef int (*lua_CFunction)(lua_State* L);
/** @brief Type for C continuation functions used with lua_pushcclosurek / lua_pcallk.
 *
 *  @param L      The Lua state.
 *  @param status Status code at the point the continuation was invoked. */
typedef int (*lua_Continuation)(lua_State* L, int status);

/*
** prototype for memory-allocation functions
*/
/** @brief Signature for custom allocator callbacks.
 *
 *  Behaves like `realloc`/`free`:
 *  - If `nsize == 0`, free the block pointed to by `ptr` and return NULL.
 *  - If `ptr == NULL`, allocate a new block of `nsize` bytes.
 *  - Otherwise, resize `ptr` from `osize` to `nsize` bytes.
 *
 *  @param ud    Opaque userdata pointer passed to lua_newstate().
 *  @param ptr   Pointer to the existing block (NULL for fresh allocations).
 *  @param osize Original size of the block in bytes (0 for fresh allocations).
 *  @param nsize Requested new size in bytes (0 to free).
 *  @return Pointer to the new block, or NULL on failure/free. */
typedef void* (*lua_Alloc)(void* ud, void* ptr, size_t osize, size_t nsize);

// non-return type
#define l_noret void LUA_NORETURN

/*
** basic types
*/
/** @brief Returned by lua_type() when the stack index is invalid (out of range). */
#define LUA_TNONE (-1)

/*
 * WARNING: if you change the order of this enumeration,
 * grep "ORDER TYPE"
 */
// clang-format off
/** @brief Luau value type tags.  Returned by lua_type() and used with lua_Type comparisons. */
enum lua_Type
{
    LUA_TNIL = 0,     ///< `nil` — must be 0 so that lua_isnoneornil works via `<= 0`.
    LUA_TBOOLEAN = 1, ///< `boolean` — must be 1 due to l_isfalse logic.

    LUA_TLIGHTUSERDATA, ///< Light userdata — an unmanaged C pointer with an optional integer tag.
    LUA_TNUMBER,        ///< Number (`double` or integer depending on context).
    LUA_TVECTOR,        ///< Vector (float2/float3/float4 depending on LUA_VECTOR_SIZE).

    LUA_TSTRING, ///< String — interned, GC-managed.  All types ABOVE this are value types; all types BELOW are GC types.

    LUA_TTABLE,    ///< Table — the universal associative container.
    LUA_TFUNCTION, ///< Function — Lua closure or C function.
    LUA_TUSERDATA, ///< Full userdata — heap block managed by the GC, with an optional integer tag.
    LUA_TTHREAD,   ///< Coroutine thread.
    LUA_TBUFFER,   ///< Buffer — raw byte array managed by the GC (Luau extension).

    // values below this line are used in GCObject tags but may never show up in TValue type tags
    LUA_TPROTO,   ///< Function prototype (internal GC object, not a stack type).
    LUA_TUPVAL,   ///< Upvalue (internal GC object, not a stack type).
    LUA_TDEADKEY, ///< Dead table key placeholder (internal).

    // the count of TValue type tags
    LUA_T_COUNT = LUA_TPROTO ///< Number of valid stack-level type tags.
};
// clang-format on

/** @brief Floating-point number type used by Luau (`double`). */
typedef double lua_Number;

/** @brief Signed integer type used by Luau (`int`). */
typedef int lua_Integer;

/** @brief Unsigned integer type used by Luau (`unsigned`). */
typedef unsigned lua_Unsigned;

/*
** state manipulation
*/

/** @brief Creates a new independent Lua state using the provided allocator.
 *
 *  `[-0, +0, -]`
 *
 *  Allocates and initialises the main thread and internal VM structures.
 *  The caller must destroy the state with lua_close() when finished.
 *
 *  @param f   Custom allocator function.
 *  @param ud  Opaque userdata forwarded to every call of @p f.
 *  @return    Pointer to the new state, or NULL if allocation failed. */
LUA_API lua_State* lua_newstate(lua_Alloc f, void* ud);

/** @brief Destroys all objects in the Lua state and releases all memory.
 *
 *  `[-0, +0, -]`
 *
 *  Calls finalizers for all userdata objects, then frees the state itself.
 *  Do not access @p L after this call.
 *
 *  @param L  The Lua state to close. */
LUA_API void lua_close(lua_State* L);

/** @brief Creates a new coroutine thread as a child of @p L.
 *
 *  `[-0, +1, m]`
 *
 *  The new thread shares the same global environment and GC with @p L.
 *  The new thread is pushed onto @p L's stack.
 *
 *  @param L  Parent Lua state.
 *  @return   Pointer to the new coroutine state. */
LUA_API lua_State* lua_newthread(lua_State* L);

/** @brief Returns the main thread of the Lua state that @p L belongs to.
 *
 *  `[-0, +0, -]`
 *
 *  @param L  Any coroutine of the target Lua state.
 *  @return   Pointer to the main thread state. */
LUA_API lua_State* lua_mainthread(lua_State* L);

/** @brief Resets a coroutine to its initial (suspended) state so it can be resumed again.
 *
 *  `[-0, +0, -]`
 *
 *  Clears the coroutine's stack and error state.  The function originally passed to
 *  lua_resume is not preserved — the caller must push a new function before resuming.
 *
 *  @param L  Coroutine state to reset. */
LUA_API void lua_resetthread(lua_State* L);

/** @brief Returns non-zero if the coroutine has been reset and not yet given a new function.
 *
 *  `[-0, +0, -]`
 *
 *  @param L  Coroutine state to query.
 *  @return   Non-zero if reset (initial state), zero otherwise. */
LUA_API int lua_isthreadreset(lua_State* L);

/*
** basic stack manipulation
*/

/** @brief Converts a possibly-relative (negative) stack index to an absolute one.
 *
 *  `[-0, +0, -]`
 *
 *  Pseudo-indices (LUA_REGISTRYINDEX etc.) are returned unchanged.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index (may be negative or positive).
 *  @return     Equivalent positive index, or the pseudo-index unchanged. */
LUA_API int lua_absindex(lua_State* L, int idx);

/** @brief Returns the number of values on the stack (i.e. the index of the top element).
 *
 *  `[-0, +0, -]`
 *
 *  @param L  The Lua state.
 *  @return   Stack top (0 means empty). */
LUA_API int lua_gettop(lua_State* L);

/** @brief Sets the stack top to @p idx, pushing nils or popping values as needed.
 *
 *  `[-?, +?, -]`
 *
 *  If @p idx is 0, all values are popped.  Negative indices are relative to the
 *  current top (e.g. -1 pops one value).
 *
 *  @param L    The Lua state.
 *  @param idx  New stack top, or negative offset from current top. */
LUA_API void lua_settop(lua_State* L, int idx);

/** @brief Pushes a copy of the value at @p idx onto the top of the stack.
 *
 *  `[-0, +1, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the value to duplicate. */
LUA_API void lua_pushvalue(lua_State* L, int idx);

/** @brief Removes the element at @p idx, shifting elements above it down.
 *
 *  `[-1, +0, -]`
 *
 *  Cannot be used with pseudo-indices.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the element to remove. */
LUA_API void lua_remove(lua_State* L, int idx);

/** @brief Moves the top of the stack element to position @p idx, shifting others up.
 *
 *  `[-1, +1, -]`
 *
 *  Cannot be used with pseudo-indices.
 *
 *  @param L    The Lua state.
 *  @param idx  Destination stack index. */
LUA_API void lua_insert(lua_State* L, int idx);

/** @brief Replaces the value at @p idx with the top of the stack, popping the top.
 *
 *  `[-1, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index to overwrite (may be a pseudo-index). */
LUA_API void lua_replace(lua_State* L, int idx);

/** @brief Ensures that at least @p sz extra stack slots are available.
 *
 *  `[-0, +0, -]`
 *
 *  @param L   The Lua state.
 *  @param sz  Number of extra slots required.
 *  @return    Non-zero on success, zero if the stack cannot grow. */
LUA_API int lua_checkstack(lua_State* L, int sz);

/** @brief Like lua_checkstack() but raises an error instead of returning 0.
 *
 *  `[-0, +0, e]`
 *
 *  Allows the stack to grow beyond the soft per-coroutine limit — use with care.
 *
 *  @param L   The Lua state.
 *  @param sz  Number of extra slots required. */
LUA_API void lua_rawcheckstack(lua_State* L, int sz); // allows for unlimited stack frames

/** @brief Moves the top @p n values from `from` to `to`, popping them from `from`.
 *
 *  `[-n, +n, -]`
 *
 *  Both states must belong to the same main thread (i.e. the same lua_newstate() call).
 *
 *  @param from  Source state; the top @p n values are removed.
 *  @param to    Destination state; the values are pushed here.
 *  @param n     Number of values to move. */
LUA_API void lua_xmove(lua_State* from, lua_State* to, int n);

/** @brief Pushes a copy of the value at @p idx in `from` onto `to` without removing it from `from`.
 *
 *  `[-0, +1, -]`
 *
 *  @note Luau extension — not present in standard Lua 5.x.
 *  Both states must belong to the same main thread.
 *
 *  @param from  Source state.
 *  @param to    Destination state.
 *  @param idx   Stack index in `from` to copy. */
LUA_API void lua_xpush(lua_State* from, lua_State* to, int idx);

/*
** access functions (stack -> C)
*/

/** @brief Returns non-zero if the value at @p idx is a number or a string coercible to a number.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index to test.
 *  @return     Non-zero if numeric. */
LUA_API int lua_isnumber(lua_State* L, int idx);

/** @brief Returns non-zero if the value at @p idx is a string or a number (which is always coercible).
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index to test.
 *  @return     Non-zero if a string or number. */
LUA_API int lua_isstring(lua_State* L, int idx);

/** @brief Returns non-zero if the value at @p idx is a C function (or C closure).
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index to test.
 *  @return     Non-zero if a C function. */
LUA_API int lua_iscfunction(lua_State* L, int idx);

/** @brief Returns non-zero if the value at @p idx is a Lua (bytecode) function.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index to test.
 *  @return     Non-zero if a Lua function/closure. */
LUA_API int lua_isLfunction(lua_State* L, int idx);

/** @brief Returns non-zero if the value at @p idx is a full userdata or light userdata.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index to test.
 *  @return     Non-zero if userdata. */
LUA_API int lua_isuserdata(lua_State* L, int idx);

/** @brief Returns the type tag of the value at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index to inspect.
 *  @return     One of the `lua_Type` enum values, or LUA_TNONE for an invalid index. */
LUA_API int lua_type(lua_State* L, int idx);

/** @brief Returns the human-readable name for type tag @p tp.
 *
 *  `[-0, +0, -]`
 *
 *  @param L   The Lua state (unused in current implementation but kept for API consistency).
 *  @param tp  A `lua_Type` integer value.
 *  @return    Pointer to a static string such as `"nil"`, `"number"`, `"string"`, etc. */
LUA_API const char* lua_typename(lua_State* L, int tp);

/** @brief Returns non-zero if the values at @p idx1 and @p idx2 are equal according to `__eq` metamethods.
 *
 *  `[-0, +0, e]`
 *
 *  May invoke `__eq` metamethods, which can raise an error.
 *
 *  @param L     The Lua state.
 *  @param idx1  Stack index of the first value.
 *  @param idx2  Stack index of the second value.
 *  @return      Non-zero if equal. */
LUA_API int lua_equal(lua_State* L, int idx1, int idx2);

/** @brief Returns non-zero if the values at @p idx1 and @p idx2 are primitively equal (no metamethods).
 *
 *  `[-0, +0, -]`
 *
 *  @param L     The Lua state.
 *  @param idx1  Stack index of the first value.
 *  @param idx2  Stack index of the second value.
 *  @return      Non-zero if primitively equal. */
LUA_API int lua_rawequal(lua_State* L, int idx1, int idx2);

/** @brief Returns non-zero if the value at @p idx1 is less than the value at @p idx2.
 *
 *  `[-0, +0, e]`
 *
 *  May invoke `__lt` metamethods, which can raise an error.
 *
 *  @param L     The Lua state.
 *  @param idx1  Stack index of the first value.
 *  @param idx2  Stack index of the second value.
 *  @return      Non-zero if idx1 < idx2. */
LUA_API int lua_lessthan(lua_State* L, int idx1, int idx2);

/** @brief Converts the value at @p idx to a `double`, with success indicator.
 *
 *  `[-0, +0, -]`
 *
 *  @param L      The Lua state.
 *  @param idx    Stack index of the value.
 *  @param isnum  If non-NULL, set to non-zero on success, zero on failure.
 *  @return       The number value, or 0 if conversion failed. */
LUA_API double lua_tonumberx(lua_State* L, int idx, int* isnum);

/** @brief Converts the value at @p idx to a `lua_Integer`, with success indicator.
 *
 *  `[-0, +0, -]`
 *
 *  @param L      The Lua state.
 *  @param idx    Stack index of the value.
 *  @param isnum  If non-NULL, set to non-zero on success, zero on failure.
 *  @return       The integer value, or 0 if conversion failed. */
LUA_API int lua_tointegerx(lua_State* L, int idx, int* isnum);

/** @brief Converts the value at @p idx to `lua_Unsigned`, with success indicator.
 *
 *  `[-0, +0, -]`
 *
 *  @param L      The Lua state.
 *  @param idx    Stack index of the value.
 *  @param isnum  If non-NULL, set to non-zero on success, zero on failure.
 *  @return       The unsigned value, or 0 if conversion failed. */
LUA_API unsigned lua_tounsignedx(lua_State* L, int idx, int* isnum);

/** @brief Returns a pointer to the float components of the vector at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  The returned pointer is valid as long as the value remains on the stack.
 *  The array has LUA_VECTOR_SIZE elements (3 or 4).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the vector value.
 *  @return     Pointer to float[LUA_VECTOR_SIZE], or NULL if not a vector. */
LUA_API const float* lua_tovector(lua_State* L, int idx);

/** @brief Converts the value at @p idx to a C boolean (any truthy value → 1, false/nil → 0).
 *
 *  `[-0, +0, -]`
 *
 *  Unlike lua_checkboolean, this never raises an error.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the value.
 *  @return     0 if the value is `false` or `nil`, 1 otherwise. */
LUA_API int lua_toboolean(lua_State* L, int idx);

/** @brief Returns a C string pointer and optional length for the string/number at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  For number values the value on the stack is replaced with the string representation.
 *  The pointer is valid as long as the value remains on the stack and is not modified.
 *  The string is NUL-terminated but may contain embedded NULs; use @p len to get the true length.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the string or number.
 *  @param len  If non-NULL, receives the string length in bytes (excluding NUL terminator).
 *  @return     Pointer to the interned string data, or NULL if value is not a string/number. */
LUA_API const char* lua_tolstring(lua_State* L, int idx, size_t* len);

/** @brief Like lua_tolstring() but also returns the string's atom id.
 *
 *  `[-0, +0, -]`
 *
 *  Atom ids are assigned by the `useratom` callback in lua_Callbacks.
 *  Returns -1 in @p atom if no atom callback is set or the string has no atom.
 *
 *  @param L     The Lua state.
 *  @param idx   Stack index of the string value.
 *  @param atom  Receives the atom id (or -1).
 *  @return      Pointer to the string data, or NULL if not a string. */
LUA_API const char* lua_tostringatom(lua_State* L, int idx, int* atom);

/** @brief Combines lua_tolstring() and lua_tostringatom() — returns length and atom together.
 *
 *  `[-0, +0, -]`
 *
 *  @param L     The Lua state.
 *  @param idx   Stack index of the string value.
 *  @param len   Receives the string length in bytes.
 *  @param atom  Receives the atom id (or -1).
 *  @return      Pointer to the string data, or NULL if not a string. */
LUA_API const char* lua_tolstringatom(lua_State* L, int idx, size_t* len, int* atom);

/** @brief Returns the method name used in the most recent NAMECALL instruction, with optional atom.
 *
 *  `[-0, +0, -]`
 *
 *  Only valid inside a C function invoked via a NAMECALL dispatch (e.g. `obj:method()`).
 *  Returns NULL if the current call was not a NAMECALL.
 *
 *  @param L     The Lua state.
 *  @param atom  If non-NULL, receives the method name's atom id.
 *  @return      The method name string, or NULL. */
LUA_API const char* lua_namecallatom(lua_State* L, int* atom);

/** @brief Returns the "length" of the value at @p idx (like the `#` operator, without metamethods).
 *
 *  `[-0, +0, -]`
 *
 *  For strings: byte length.  For tables: border (sequence length).
 *  Does not invoke `__len` metamethods.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the value.
 *  @return     Integer length. */
LUA_API int lua_objlen(lua_State* L, int idx);

/** @brief Returns the C function pointer for the function at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the C function value.
 *  @return     The function pointer, or NULL if not a C function. */
LUA_API lua_CFunction lua_tocfunction(lua_State* L, int idx);

/** @brief Returns the pointer stored in the light userdata at @p idx (tag-agnostic).
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the light userdata.
 *  @return     The raw pointer, or NULL if not a light userdata. */
LUA_API void* lua_tolightuserdata(lua_State* L, int idx);

/** @brief Returns the pointer stored in the light userdata at @p idx only if its tag matches @p tag.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the light userdata.
 *  @param tag  Expected tag (0..LUA_LUTAG_LIMIT-1).
 *  @return     The raw pointer, or NULL if not a tagged light userdata with the given tag. */
LUA_API void* lua_tolightuserdatatagged(lua_State* L, int idx, int tag);

/** @brief Returns the pointer to the full userdata block at @p idx (tag-agnostic).
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the userdata.
 *  @return     Pointer to the userdata memory block, or NULL if not userdata. */
LUA_API void* lua_touserdata(lua_State* L, int idx);

/** @brief Returns the pointer to the full userdata at @p idx only if its tag matches @p tag.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the userdata.
 *  @param tag  Expected tag (0..LUA_UTAG_LIMIT-1).
 *  @return     Pointer to the userdata memory block, or NULL on tag mismatch / not userdata. */
LUA_API void* lua_touserdatatagged(lua_State* L, int idx, int tag);

/** @brief Returns the integer tag of the full userdata at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the userdata.
 *  @return     The tag value (0..LUA_UTAG_LIMIT-1), or -1 if not a full userdata. */
LUA_API int lua_userdatatag(lua_State* L, int idx);

/** @brief Returns the integer tag of the light userdata at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the light userdata.
 *  @return     The tag value (0..LUA_LUTAG_LIMIT-1), or -1 if not a light userdata. */
LUA_API int lua_lightuserdatatag(lua_State* L, int idx);

/** @brief Returns the coroutine state pointer for the thread value at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the thread value.
 *  @return     Pointer to the coroutine state, or NULL if not a thread. */
LUA_API lua_State* lua_tothread(lua_State* L, int idx);

/** @brief Returns a pointer to the raw bytes of the buffer at @p idx, with its size.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the buffer value.
 *  @param len  If non-NULL, receives the buffer size in bytes.
 *  @return     Pointer to the buffer data, or NULL if not a buffer. */
LUA_API void* lua_tobuffer(lua_State* L, int idx, size_t* len);

/** @brief Returns a unique C pointer that identifies the GC object at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  Useful only for identity comparisons.  For strings the interned pointer is returned.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     Opaque pointer, or NULL for primitives (nil, bool, number). */
LUA_API const void* lua_topointer(lua_State* L, int idx);

/*
** push functions (C -> stack)
*/

/** @brief Pushes `nil` onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  @param L  The Lua state. */
LUA_API void lua_pushnil(lua_State* L);

/** @brief Pushes a `double` number onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  @param L  The Lua state.
 *  @param n  The number value to push. */
LUA_API void lua_pushnumber(lua_State* L, double n);

/** @brief Pushes an integer (promoted to double) onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  @param L  The Lua state.
 *  @param n  The integer value to push. */
LUA_API void lua_pushinteger(lua_State* L, int n);

/** @brief Pushes an unsigned integer (promoted to double) onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  @param L  The Lua state.
 *  @param n  The unsigned value to push. */
LUA_API void lua_pushunsigned(lua_State* L, unsigned n);

#if LUA_VECTOR_SIZE == 4
/** @brief Pushes a 4-component float vector onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  Only available when LUA_VECTOR_SIZE == 4.
 *
 *  @param L  The Lua state.
 *  @param x  First component.
 *  @param y  Second component.
 *  @param z  Third component.
 *  @param w  Fourth component. */
LUA_API void lua_pushvector(lua_State* L, float x, float y, float z, float w);
#else
/** @brief Pushes a 3-component float vector onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  Only available when LUA_VECTOR_SIZE == 3 (the default).
 *
 *  @param L  The Lua state.
 *  @param x  First component.
 *  @param y  Second component.
 *  @param z  Third component. */
LUA_API void lua_pushvector(lua_State* L, float x, float y, float z);
#endif

/** @brief Pushes a copy of the string @p s with explicit byte length @p l.
 *
 *  `[-0, +1, m]`
 *
 *  The string is interned by the VM and may contain embedded NUL bytes.
 *
 *  @param L  The Lua state.
 *  @param s  Pointer to the string data (does not need to be NUL-terminated).
 *  @param l  Length in bytes. */
LUA_API void lua_pushlstring(lua_State* L, const char* s, size_t l);

/** @brief Pushes a NUL-terminated C string onto the stack.
 *
 *  `[-0, +1, m]`
 *
 *  @param L  The Lua state.
 *  @param s  NUL-terminated C string to intern and push. */
LUA_API void lua_pushstring(lua_State* L, const char* s);

/** @brief Formats a string with `va_list` and pushes the result onto the stack.
 *
 *  `[-0, +1, m]`
 *
 *  Supported format specifiers: `%d`, `%i`, `%u`, `%o`, `%x`, `%X`, `%e`, `%E`, `%f`, `%g`, `%G`,
 *  `%c`, `%s`, `%p`, `%%`, `%q`.  No width/precision modifiers.
 *
 *  @param L     The Lua state.
 *  @param fmt   Format string.
 *  @param argp  Argument list.
 *  @return      Pointer to the resulting string on the stack. */
LUA_API const char* lua_pushvfstring(lua_State* L, const char* fmt, va_list argp);

/** @brief Formats a string with variadic arguments and pushes the result.
 *
 *  `[-0, +1, m]`
 *
 *  Same format specifier set as lua_pushvfstring().  Prefer the lua_pushfstring() macro
 *  which forwards `__VA_ARGS__` conveniently.
 *
 *  @param L    The Lua state.
 *  @param fmt  Format string.
 *  @return     Pointer to the resulting string on the stack. */
LUA_API LUA_PRINTF_ATTR(2, 3) const char* lua_pushfstringL(lua_State* L, const char* fmt, ...);

/** @brief Pushes a new C closure with an optional continuation and upvalues.
 *
 *  `[-nup, +1, m]`
 *
 *  Pops @p nup values from the stack to use as upvalues.  The closure will call @p fn when invoked.
 *  @p cont is a Luau-specific continuation function (called when the coroutine is resumed after a
 *  yield that occurred inside @p fn via lua_yield/lua_pcallk); pass NULL if no continuation is needed.
 *
 *  @note Luau extension: the @p cont parameter does not exist in standard Lua 5.x.
 *
 *  @param L          The Lua state.
 *  @param fn         The C function body.
 *  @param debugname  Name used in error messages and debug info (may be NULL).
 *  @param nup        Number of upvalues to pop from the stack and bind to the closure.
 *  @param cont       Continuation function, or NULL. */
LUA_API void lua_pushcclosurek(lua_State* L, lua_CFunction fn, const char* debugname, int nup, lua_Continuation cont);

/** @brief Pushes a boolean value onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  @param L  The Lua state.
 *  @param b  Any non-zero value pushes `true`; 0 pushes `false`. */
LUA_API void lua_pushboolean(lua_State* L, int b);

/** @brief Pushes the current thread (coroutine) onto its own stack.
 *
 *  `[-0, +1, -]`
 *
 *  @param L  The Lua state.
 *  @return   1 if @p L is the main thread, 0 if it is a coroutine. */
LUA_API int lua_pushthread(lua_State* L);

/** @brief Pushes a tagged light userdata pointer onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  Light userdata is an unmanaged C pointer; the tag (0..LUA_LUTAG_LIMIT-1) lets the host
 *  distinguish different pointer types without allocating full userdata.
 *
 *  @param L    The Lua state.
 *  @param p    The C pointer to wrap.
 *  @param tag  Integer tag identifying the pointer type. */
LUA_API void lua_pushlightuserdatatagged(lua_State* L, void* p, int tag);

/** @brief Allocates a new GC-managed userdata block and pushes it.
 *
 *  `[-0, +1, m]`
 *
 *  The returned block is zero-initialised.  The tag (0..LUA_UTAG_LIMIT-1) lets C code
 *  distinguish between different userdata types at runtime.
 *  Use `lua_newuserdata(L, sz)` (macro) for tag 0.
 *
 *  @param L    The Lua state.
 *  @param sz   Size of the userdata block in bytes.
 *  @param tag  Integer tag (0..LUA_UTAG_LIMIT-1).
 *  @return     Pointer to the allocated userdata block. */
LUA_API void* lua_newuserdatatagged(lua_State* L, size_t sz, int tag);

/** @brief Allocates a tagged userdata and automatically assigns the metatable registered for @p tag.
 *
 *  `[-0, +1, m]`
 *
 *  The metatable must have been previously stored with lua_setuserdatametatable().
 *  Equivalent to lua_newuserdatatagged() + setmetatable in one atomic step.
 *
 *  @param L    The Lua state.
 *  @param sz   Size of the userdata block in bytes.
 *  @param tag  Integer tag whose metatable to fetch.
 *  @return     Pointer to the allocated userdata block. */
LUA_API void* lua_newuserdatataggedwithmetatable(lua_State* L, size_t sz, int tag); // metatable fetched with lua_getuserdatametatable

/** @brief Allocates a userdata block with an inline C destructor.
 *
 *  `[-0, +1, m]`
 *
 *  The destructor receives the raw block pointer and is called when the GC collects the object.
 *  Unlike tag-based destructors (lua_setuserdatadtor), this destructor is stored per-object.
 *
 *  @param L     The Lua state.
 *  @param sz    Size of the userdata block in bytes.
 *  @param dtor  Destructor called with the raw block pointer on GC collection. */
LUA_API void* lua_newuserdatadtor(lua_State* L, size_t sz, void (*dtor)(void*));

/** @brief Allocates a new GC-managed raw byte buffer and pushes it.
 *
 *  `[-0, +1, m]`
 *
 *  The buffer is zero-initialised.  It is a first-class Luau value (type LUA_TBUFFER).
 *
 *  @param L   The Lua state.
 *  @param sz  Size of the buffer in bytes.
 *  @return    Pointer to the raw buffer memory. */
LUA_API void* lua_newbuffer(lua_State* L, size_t sz);

/*
** get functions (Lua -> stack)
*/

/** @brief Pushes `t[k]` where `t` is the table at @p idx and `k` is the value at the top of the stack.
 *
 *  `[-1, +1, e]`
 *
 *  Pops the key.  May invoke `__index` metamethods, which can raise errors.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table (or object with __index).
 *  @return     The type of the pushed value. */
LUA_API int lua_gettable(lua_State* L, int idx);

/** @brief Pushes `t[k]` where `t` is the value at @p idx and `k` is the string @p k.
 *
 *  `[-0, +1, e]`
 *
 *  May invoke `__index` metamethods.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table or object.
 *  @param k    Field name (NUL-terminated).
 *  @return     The type of the pushed value. */
LUA_API int lua_getfield(lua_State* L, int idx, const char* k);

/** @brief Pushes `t[k]` using a raw table access — no metamethods.
 *
 *  `[-0, +1, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param k    Field name (NUL-terminated).
 *  @return     The type of the pushed value. */
LUA_API int lua_rawgetfield(lua_State* L, int idx, const char* k);

/** @brief Pushes `t[k]` where `k` is the value at the top of the stack — no metamethods.
 *
 *  `[-1, +1, -]`
 *
 *  Pops the key.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @return     The type of the pushed value. */
LUA_API int lua_rawget(lua_State* L, int idx);

/** @brief Pushes `t[n]` — integer key raw access, no metamethods.
 *
 *  `[-0, +1, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param n    Integer key.
 *  @return     The type of the pushed value. */
LUA_API int lua_rawgeti(lua_State* L, int idx, int n);

/** @brief Pushes `t[p]` using a tagged light userdata pointer as key — no metamethods.
 *
 *  `[-0, +1, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param p    Light userdata pointer key.
 *  @param tag  Tag for the light userdata key.
 *  @return     The type of the pushed value. */
LUA_API int lua_rawgetptagged(lua_State* L, int idx, void* p, int tag);

/** @brief Creates a new empty table and pushes it.
 *
 *  `[-0, +1, m]`
 *
 *  Pre-allocates space for @p narr array entries and @p nrec hash entries to avoid rehashing.
 *  Use `lua_newtable(L)` (macro) for a fully empty table.
 *
 *  @param L     The Lua state.
 *  @param narr  Hint: number of expected array-part entries.
 *  @param nrec  Hint: number of expected hash-part entries. */
LUA_API void lua_createtable(lua_State* L, int narr, int nrec);

/** @brief Sets or clears the read-only flag on the table at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  A read-only table raises an error on any write attempt from Lua code.
 *
 *  @param L        The Lua state.
 *  @param idx      Stack index of the table.
 *  @param enabled  Non-zero to make read-only, zero to make writable. */
LUA_API void lua_setreadonly(lua_State* L, int idx, int enabled);

/** @brief Returns the read-only flag of the table at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @return     Non-zero if the table is read-only, zero otherwise. */
LUA_API int lua_getreadonly(lua_State* L, int idx);

/** @brief Sets the safe-env flag on the table at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  When a table is marked as a safe environment, the VM can apply certain optimisations
 *  (e.g. assuming globals are not modified by untrusted code).
 *
 *  @param L        The Lua state.
 *  @param idx      Stack index of the table.
 *  @param enabled  Non-zero to mark safe, zero to clear. */
LUA_API void lua_setsafeenv(lua_State* L, int idx, int enabled);

/** @brief Pushes the metatable of the object at @p objindex, or nothing if there is none.
 *
 *  `[-0, +1|0, -]`
 *
 *  @param L          The Lua state.
 *  @param objindex   Stack index of the object.
 *  @return           Non-zero and pushes the metatable, or returns 0 and pushes nothing. */
LUA_API int lua_getmetatable(lua_State* L, int objindex);

/** @brief Pushes the environment (upvalue table) of the function or thread at @p idx.
 *
 *  `[-0, +1, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the function or thread. */
LUA_API void lua_getfenv(lua_State* L, int idx);

/*
** set functions (stack -> Lua)
*/

/** @brief Sets `t[k] = v` where `t` is at @p idx, `k` is at stack top-1, and `v` is at stack top.
 *
 *  `[-2, +0, e]`
 *
 *  Pops both key and value.  May invoke `__newindex` metamethods.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table (or object with __newindex). */
LUA_API void lua_settable(lua_State* L, int idx);

/** @brief Sets `t[k] = v` where `t` is at @p idx, `k` is @p k, and `v` is at stack top.
 *
 *  `[-1, +0, e]`
 *
 *  Pops the value.  May invoke `__newindex` metamethods.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table or object.
 *  @param k    Field name (NUL-terminated). */
LUA_API void lua_setfield(lua_State* L, int idx, const char* k);

/** @brief Sets `t[k] = v` using a raw table write — no metamethods.
 *
 *  `[-1, +0, m]`
 *
 *  Pops the value.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param k    Field name (NUL-terminated). */
LUA_API void lua_rawsetfield(lua_State* L, int idx, const char* k);

/** @brief Sets `t[k] = v` where `k` is at top-1 and `v` is at top — no metamethods.
 *
 *  `[-2, +0, m]`
 *
 *  Pops key and value.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table. */
LUA_API void lua_rawset(lua_State* L, int idx);

/** @brief Sets `t[n] = v` where `v` is at the top of the stack — no metamethods.
 *
 *  `[-1, +0, m]`
 *
 *  Pops the value.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param n    Integer key. */
LUA_API void lua_rawseti(lua_State* L, int idx, int n);

/** @brief Sets `t[p] = v` using a tagged light userdata pointer key — no metamethods.
 *
 *  `[-1, +0, m]`
 *
 *  Pops the value.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param p    Light userdata pointer key.
 *  @param tag  Tag for the light userdata key. */
LUA_API void lua_rawsetptagged(lua_State* L, int idx, void* p, int tag);

/** @brief Pops a table from the stack and sets it as the metatable of the object at @p objindex.
 *
 *  `[-1, +0, -]`
 *
 *  @param L          The Lua state.
 *  @param objindex   Stack index of the object to receive the metatable.
 *  @return           Always 1 (kept for Lua 5.x API compatibility). */
LUA_API int lua_setmetatable(lua_State* L, int objindex);

/** @brief Pops a table from the stack and sets it as the environment of the function/thread at @p idx.
 *
 *  `[-1, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the function or thread.
 *  @return     Non-zero on success. */
LUA_API int lua_setfenv(lua_State* L, int idx);

/*
** `load' and `call' functions (load and run Luau bytecode)
*/

/** @brief Loads pre-compiled Luau bytecode and pushes the resulting function closure.
 *
 *  `[-0, +1, -]`
 *
 *  @note Luau-specific: accepts compiled bytecode ONLY — NOT Lua source text.
 *  Use the Luau compiler (luau_compile in luacode.h) to produce bytecode first.
 *
 *  @param L          The Lua state.
 *  @param chunkname  Name shown in error messages and debug info (e.g. "@filename").
 *  @param data       Pointer to the bytecode buffer.
 *  @param size       Length of the bytecode buffer in bytes.
 *  @param env        Stack index of the environment table to use, or 0 for the default globals.
 *  @return           LUA_OK (0) on success, or a non-zero error code if the bytecode is invalid. */
LUA_API int luau_load(lua_State* L, const char* chunkname, const char* data, size_t size, int env);

/** @brief Calls the function at the top of the stack, passing @p nargs arguments.
 *
 *  `[-(nargs+1), +nresults, e]`
 *
 *  Identical semantics to Lua 5.x lua_call.  The function and arguments are popped;
 *  @p nresults return values are pushed.  Use LUA_MULTRET for all results.
 *  Errors propagate as C++ exceptions (or longjmp depending on build config).
 *
 *  @param L        The Lua state.
 *  @param nargs    Number of arguments on the stack above the function.
 *  @param nresults Number of results to leave on the stack, or LUA_MULTRET. */
LUA_API void lua_call(lua_State* L, int nargs, int nresults);

/** @brief Protected call: calls the function at the stack, catching errors.
 *
 *  `[-(nargs+1), +nresults|1, -]`
 *
 *  On error, all intermediate stack values are removed and the error object is pushed.
 *  @p errfunc is the stack index of an error-handler function (0 for none).
 *
 *  @param L        The Lua state.
 *  @param nargs    Number of arguments on the stack above the function.
 *  @param nresults Number of results to leave on the stack, or LUA_MULTRET.
 *  @param errfunc  Stack index of error-handler function, or 0.
 *  @return         LUA_OK on success, or an error code (LUA_ERRRUN, LUA_ERRMEM, LUA_ERRERR). */
LUA_API int lua_pcall(lua_State* L, int nargs, int nresults, int errfunc);

/** @brief Calls @p func in protected mode, passing @p ud as a light userdata argument.
 *
 *  `[-0, +0, -]`
 *
 *  A safe wrapper for calling C functions that may raise Lua errors.
 *  The function receives @p ud via lua_touserdata(L, 1).
 *
 *  @param L     The Lua state.
 *  @param func  C function to call.
 *  @param ud    Userdata pointer passed as the first argument.
 *  @return      LUA_OK on success, or an error code. */
LUA_API int lua_cpcall(lua_State* L, lua_CFunction func, void* ud);

/*
** coroutine functions
*/

/** @brief Suspends the current coroutine, returning @p nresults values to the resumer.
 *
 *  `[-nresults, +?, v]`
 *
 *  Must be called from a coroutine (not the main thread).  The @p nresults values at the
 *  top of the stack are returned to lua_resume().  When the coroutine is resumed, the
 *  values passed to lua_resume() are pushed onto this coroutine's stack.
 *
 *  @param L        The Lua state (coroutine).
 *  @param nresults Number of values to yield to the resumer.
 *  @return         The number of values received when resumed (for use in a continuation). */
LUA_API int lua_yield(lua_State* L, int nresults);

/** @brief Suspends the current coroutine at a debug breakpoint.
 *
 *  `[-0, +0, -]`
 *
 *  Triggers the `debugbreak` callback and sets status to LUA_BREAK.
 *  The coroutine can be resumed normally after inspection.
 *
 *  @param L  The Lua state.
 *  @return   Always 0. */
LUA_API int lua_break(lua_State* L);

/** @brief Starts or continues execution of a suspended coroutine.
 *
 *  `[-narg, +nresults, -]`
 *
 *  To start a coroutine push the function then @p narg arguments and call lua_resume().
 *  To resume after a yield, push @p narg values (delivered to the yield point) and call again.
 *
 *  @param L     The coroutine to resume.
 *  @param from  The calling coroutine (may be NULL for the main thread).
 *  @param narg  Number of arguments (start) or resume values (after yield).
 *  @return      LUA_OK if the coroutine finished, LUA_YIELD if it yielded again, or an error code. */
LUA_API int lua_resume(lua_State* L, lua_State* from, int narg);

/** @brief Resumes a coroutine with an error, as if it had raised the value at the top of @p from's stack.
 *
 *  `[-1, +0, -]`
 *
 *  Pops the error object from @p from's stack and injects it into @p L as an error.
 *
 *  @param L     The coroutine to receive the error.
 *  @param from  State whose stack top is used as the error object.
 *  @return      An error status code. */
LUA_API int lua_resumeerror(lua_State* L, lua_State* from);

/** @brief Returns the status of the coroutine @p L.
 *
 *  `[-0, +0, -]`
 *
 *  @param L  The coroutine.
 *  @return   LUA_OK (running/suspended-at-start), LUA_YIELD (suspended), or an error code. */
LUA_API int lua_status(lua_State* L);

/** @brief Returns non-zero if the coroutine @p L can yield at the current point.
 *
 *  `[-0, +0, -]`
 *
 *  @param L  The Lua state.
 *  @return   Non-zero if yieldable. */
LUA_API int lua_isyieldable(lua_State* L);

/** @brief Returns the opaque per-thread userdata pointer stored on @p L.
 *
 *  `[-0, +0, -]`
 *
 *  @param L  The Lua state.
 *  @return   The stored pointer, or NULL if never set. */
LUA_API void* lua_getthreaddata(lua_State* L);

/** @brief Stores an opaque per-thread userdata pointer on @p L.
 *
 *  `[-0, +0, -]`
 *
 *  The VM never reads or writes this pointer; it is entirely for host use.
 *
 *  @param L     The Lua state.
 *  @param data  Pointer to store. */
LUA_API void lua_setthreaddata(lua_State* L, void* data);

/** @brief Returns the coroutine status of @p co as seen from @p L.
 *
 *  `[-0, +0, -]`
 *
 *  @param L   Observer state (used to detect the "normal" status).
 *  @param co  Coroutine to query.
 *  @return    One of the lua_CoStatus enum values. */
LUA_API int lua_costatus(lua_State* L, lua_State* co);

/*
** garbage-collection function and options
*/

/** @brief GC operation codes for lua_gc(). */
enum lua_GCOp
{
    // stop and resume incremental garbage collection
    LUA_GCSTOP,    ///< Pause the incremental GC (no automatic collection steps).
    LUA_GCRESTART, ///< Resume the incremental GC after a LUA_GCSTOP.

    // run a full GC cycle; not recommended for latency sensitive applications
    LUA_GCCOLLECT, ///< Force a complete GC cycle immediately.  Avoid in latency-sensitive code.

    // return the heap size in KB and the remainder in bytes
    LUA_GCCOUNT,  ///< Return total heap size in KB (pass to lua_gc as `what`).
    LUA_GCCOUNTB, ///< Return remainder bytes on top of LUA_GCCOUNT KB.

    // return 1 if GC is active (not stopped); note that GC may not be actively collecting even if it's running
    LUA_GCISRUNNING, ///< Return 1 if the GC is running (not stopped via LUA_GCSTOP).

    /*
    ** perform an explicit GC step, with the step size specified in KB
    **
    ** garbage collection is handled by 'assists' that perform some amount of GC work matching pace of allocation
    ** explicit GC steps allow to perform some amount of work at custom points to offset the need for GC assists
    ** note that GC might also be paused for some duration (until bytes allocated meet the threshold)
    ** if an explicit step is performed during this pause, it will trigger the start of the next collection cycle
    */
    LUA_GCSTEP, ///< Perform a GC step of `data` KB.  Helps to spread GC cost over time.

    /*
    ** tune GC parameters G (goal), S (step multiplier) and step size (usually best left ignored)
    **
    ** garbage collection is incremental and tries to maintain the heap size to balance memory and performance overhead
    ** this overhead is determined by G (goal) which is the ratio between total heap size and the amount of live data in it
    ** G is specified in percentages; by default G=200% which means that the heap is allowed to grow to ~2x the size of live data.
    **
    ** collector tries to collect S% of allocated bytes by interrupting the application after step size bytes were allocated.
    ** when S is too small, collector may not be able to catch up and the effective goal that can be reached will be larger.
    ** S is specified in percentages; by default S=200% which means that collector will run at ~2x the pace of allocations.
    **
    ** it is recommended to set S in the interval [100 / (G - 100), 100 + 100 / (G - 100))] with a minimum value of 150%; for example:
    ** - for G=200%, S should be in the interval [150%, 200%]
    ** - for G=150%, S should be in the interval [200%, 300%]
    ** - for G=125%, S should be in the interval [400%, 500%]
    */
    LUA_GCSETGOAL,    ///< Set GC goal G (heap-to-live ratio in %). Default 200.
    LUA_GCSETSTEPMUL, ///< Set GC step multiplier S (collector pace in %). Default 200.
    LUA_GCSETSTEPSIZE,///< Set the allocation step size (KB) that triggers a GC assist. Usually best left at default.
};

/** @brief Controls or queries the garbage collector.
 *
 *  `[-0, +0, e]`
 *
 *  @param L     The Lua state.
 *  @param what  A lua_GCOp value specifying the operation.
 *  @param data  Integer parameter whose meaning depends on @p what (e.g. step size in KB for LUA_GCSTEP).
 *  @return      Integer result whose meaning depends on @p what (e.g. heap KB for LUA_GCCOUNT). */
LUA_API int lua_gc(lua_State* L, int what, int data);

/*
** memory statistics
** all allocated bytes are attributed to the memory category of the running thread (0..LUA_MEMORY_CATEGORIES-1)
*/

/** @brief Sets the memory category of the current thread for allocation tracking.
 *
 *  `[-0, +0, -]`
 *
 *  All subsequent allocations made by @p L are attributed to @p category.
 *  Categories are in range [0, LUA_MEMORY_CATEGORIES-1].
 *
 *  @param L         The Lua state.
 *  @param category  Integer category index. */
LUA_API void lua_setmemcat(lua_State* L, int category);

/** @brief Returns the total bytes currently allocated in @p category (or all categories if -1).
 *
 *  `[-0, +0, -]`
 *
 *  @param L         The Lua state.
 *  @param category  Category index (0..LUA_MEMORY_CATEGORIES-1), or -1 for all categories combined.
 *  @return          Total allocated bytes. */
LUA_API size_t lua_totalbytes(lua_State* L, int category);

/*
** miscellaneous functions
*/

/** @brief Raises the value at the top of the stack as a Lua error.
 *
 *  `[-1, +0, e]`
 *
 *  This function never returns.  The error value can be of any type.
 *
 *  @param L  The Lua state. */
LUA_API l_noret lua_error(lua_State* L);

/** @brief Advances a table iterator: pops a key, pushes the next key-value pair.
 *
 *  `[-1, +2|0, e]`
 *
 *  Push `nil` first to start iteration.  Returns 0 (and pushes nothing) when iteration ends.
 *  Modifying the table during iteration has undefined behaviour for hash part entries.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table to iterate.
 *  @return     Non-zero if a key-value pair was pushed, 0 at end of table. */
LUA_API int lua_next(lua_State* L, int idx);

/** @brief Low-level table iterator using an integer cursor — faster than lua_next().
 *
 *  `[-0, +2|0, -]`
 *
 *  @note Luau extension — not present in standard Lua 5.x.
 *
 *  Pass @p iter = 0 to start iteration.  Each successful call pushes the next key and value
 *  and returns the new cursor value to pass to the next call.  Returns -1 (and pushes nothing)
 *  when the table is exhausted.
 *
 *  Unlike lua_next(), this does NOT pop a key from the stack.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table to iterate.
 *  @param iter Integer cursor; start with 0, then use the returned value.
 *  @return     New cursor value (pass to next call), or -1 when done. */
LUA_API int lua_rawiter(lua_State* L, int idx, int iter);

/** @brief Concatenates the @p n values at the top of the stack, leaving one string result.
 *
 *  `[-n, +1, e]`
 *
 *  May invoke `__concat` metamethods.  If @p n is 0, pushes an empty string.
 *
 *  @param L  The Lua state.
 *  @param n  Number of values to concatenate. */
LUA_API void lua_concat(lua_State* L, int n);

/** @brief Obfuscates a pointer value for use in `tostring()` output.
 *
 *  `[-0, +0, -]`
 *
 *  Applies an XOR mask derived from the state so that raw addresses are not exposed.
 *
 *  @param L  The Lua state.
 *  @param p  Raw pointer value as a uintptr_t.
 *  @return   Obfuscated pointer value. */
LUA_API uintptr_t lua_encodepointer(lua_State* L, uintptr_t p);

/** @brief Returns a monotonically increasing high-resolution timer value in seconds.
 *
 *  `[-0, +0, -]`
 *
 *  The epoch is unspecified; useful only for measuring elapsed time.
 *
 *  @return Current time in seconds. */
LUA_API double lua_clock();

/** @brief Changes the integer tag of the full userdata at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the userdata.
 *  @param tag  New tag value (0..LUA_UTAG_LIMIT-1). */
LUA_API void lua_setuserdatatag(lua_State* L, int idx, int tag);

/** @brief Destructor callback type for tagged userdata.
 *
 *  Called by the GC when a userdata with this tag is collected.
 *  @p userdata is the raw memory block pointer. */
typedef void (*lua_Destructor)(lua_State* L, void* userdata);

/** @brief Registers a GC destructor for all userdata objects with the given integer tag.
 *
 *  `[-0, +0, -]`
 *
 *  The destructor is called whenever a tagged userdata with @p tag is collected.
 *  Pass NULL to clear a previously registered destructor.
 *
 *  @param L    The Lua state.
 *  @param tag  Userdata tag (0..LUA_UTAG_LIMIT-1).
 *  @param dtor Destructor function, or NULL to remove. */
LUA_API void lua_setuserdatadtor(lua_State* L, int tag, lua_Destructor dtor);

/** @brief Returns the destructor registered for userdata tag @p tag.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param tag  Userdata tag.
 *  @return     The registered destructor, or NULL if none. */
LUA_API lua_Destructor lua_getuserdatadtor(lua_State* L, int tag);

/** @brief Pops a metatable from the stack and associates it with userdata tag @p tag.
 *
 *  `[-1, +0, -]`
 *
 *  Used in conjunction with lua_newuserdatataggedwithmetatable() so that tagged userdata
 *  objects automatically receive the correct metatable at construction time.
 *  The metatable must have been created with luaL_newmetatable().
 *
 *  @param L    The Lua state.
 *  @param tag  Userdata tag (0..LUA_UTAG_LIMIT-1). */
// alternative access for metatables already registered with luaL_newmetatable
// used by lua_newuserdatataggedwithmetatable to create tagged userdata with the associated metatable assigned
LUA_API void lua_setuserdatametatable(lua_State* L, int tag);

/** @brief Pushes the metatable associated with userdata tag @p tag.
 *
 *  `[-0, +1, -]`
 *
 *  @param L    The Lua state.
 *  @param tag  Userdata tag (0..LUA_UTAG_LIMIT-1). */
LUA_API void lua_getuserdatametatable(lua_State* L, int tag);

/** @brief Associates a human-readable name with a light userdata tag.
 *
 *  `[-0, +0, -]`
 *
 *  The name is used in error messages when a light userdata type mismatch occurs.
 *
 *  @param L     The Lua state.
 *  @param tag   Light userdata tag (0..LUA_LUTAG_LIMIT-1).
 *  @param name  NUL-terminated name string (the pointer must remain valid for the lifetime of the state). */
LUA_API void lua_setlightuserdataname(lua_State* L, int tag, const char* name);

/** @brief Returns the name registered for a light userdata tag.
 *
 *  `[-0, +0, -]`
 *
 *  @param L    The Lua state.
 *  @param tag  Light userdata tag.
 *  @return     The registered name, or NULL if none. */
LUA_API const char* lua_getlightuserdataname(lua_State* L, int tag);

/** @brief Pushes a shallow clone of the function at @p idx.
 *
 *  `[-0, +1, m]`
 *
 *  @note Luau extension — not present in standard Lua 5.x.
 *
 *  Creates a new closure that shares the same prototype (bytecode/upvalue layout) as the
 *  original, but has independent upvalue cells.  Upvalue values are copied shallowly.
 *  Only works on Lua closures (not C functions).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the Lua function to clone. */
LUA_API void lua_clonefunction(lua_State* L, int idx);

/** @brief Removes all key-value entries from the table at @p idx.
 *
 *  `[-0, +0, -]`
 *
 *  @note Luau extension — not present in standard Lua 5.x.
 *
 *  Equivalent to iterating the table and setting each key to nil, but faster.
 *  Does not shrink the allocated memory.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table to clear. */
LUA_API void lua_cleartable(lua_State* L, int idx);

/** @brief Pushes a shallow clone of the table at @p idx.
 *
 *  `[-0, +1, m]`
 *
 *  @note Luau extension — not present in standard Lua 5.x.
 *
 *  Creates a new table containing the same key-value pairs as the source (values are not
 *  deep-copied).  The metatable is NOT copied to the clone.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table to clone. */
LUA_API void lua_clonetable(lua_State* L, int idx);

/** @brief Returns the allocator function and userdata for the Lua state.
 *
 *  `[-0, +0, -]`
 *
 *  @param L   The Lua state.
 *  @param ud  If non-NULL, receives the allocator's opaque userdata pointer.
 *  @return    The allocator function originally passed to lua_newstate(). */
LUA_API lua_Alloc lua_getallocf(lua_State* L, void** ud);

/*
** reference system, can be used to pin objects
*/
/** @brief Sentinel returned when no reference was stored (or after lua_unref). */
#define LUA_NOREF -1
/** @brief Sentinel reference for `nil` (lua_ref on a nil value returns this). */
#define LUA_REFNIL 0

/** @brief Creates a registry reference to the value at @p idx, preventing GC collection.
 *
 *  `[-0, +0, m]`
 *
 *  @note In Luau the reference is a positive integer stored in the registry table at that key.
 *  This is different from Lua 5.x where references are also integers but stored differently.
 *
 *  Use `lua_getref(L, ref)` (macro) to push the referenced value, and lua_unref() to release it.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the value to reference.
 *  @return     A positive integer reference key, or LUA_REFNIL if the value is nil. */
LUA_API int lua_ref(lua_State* L, int idx);

/** @brief Releases a registry reference created by lua_ref(), allowing the object to be GC'd.
 *
 *  `[-0, +0, -]`
 *
 *  Passing LUA_NOREF or LUA_REFNIL is a no-op.
 *
 *  @param L    The Lua state.
 *  @param ref  Reference integer returned by lua_ref(). */
LUA_API void lua_unref(lua_State* L, int ref);

/** @brief Pushes the value associated with reference @p ref onto the stack.
 *
 *  `[-0, +1, -]`
 *
 *  Expands to `lua_rawgeti(L, LUA_REGISTRYINDEX, ref)`. */
#define lua_getref(L, ref) lua_rawgeti(L, LUA_REGISTRYINDEX, (ref))

/*
** ===============================================================
** some useful macros
** ===============================================================
*/
/** @brief Converts the value at index @p i to a `lua_Number` (double), returning 0 on failure. */
#define lua_tonumber(L, i) lua_tonumberx(L, i, NULL)
/** @brief Converts the value at index @p i to a `lua_Integer`, returning 0 on failure. */
#define lua_tointeger(L, i) lua_tointegerx(L, i, NULL)
/** @brief Converts the value at index @p i to a `lua_Unsigned`, returning 0 on failure. */
#define lua_tounsigned(L, i) lua_tounsignedx(L, i, NULL)

/** @brief Pops @p n values from the stack.  Equivalent to `lua_settop(L, -(n)-1)`. */
#define lua_pop(L, n) lua_settop(L, -(n)-1)

/** @brief Creates a new empty table and pushes it.  Equivalent to `lua_createtable(L, 0, 0)`. */
#define lua_newtable(L) lua_createtable(L, 0, 0)
/** @brief Creates a new userdata with tag 0 and pushes it.  Equivalent to `lua_newuserdatatagged(L, s, 0)`. */
#define lua_newuserdata(L, s) lua_newuserdatatagged(L, s, 0)

/** @brief Returns the byte length of the string at index @p i.  Alias for `lua_objlen`. */
#define lua_strlen(L, i) lua_objlen(L, (i))

/** @brief Returns non-zero if the value at @p n is a function (Lua or C). */
#define lua_isfunction(L, n) (lua_type(L, (n)) == LUA_TFUNCTION)
/** @brief Returns non-zero if the value at @p n is a table. */
#define lua_istable(L, n) (lua_type(L, (n)) == LUA_TTABLE)
/** @brief Returns non-zero if the value at @p n is light userdata. */
#define lua_islightuserdata(L, n) (lua_type(L, (n)) == LUA_TLIGHTUSERDATA)
/** @brief Returns non-zero if the value at @p n is nil. */
#define lua_isnil(L, n) (lua_type(L, (n)) == LUA_TNIL)
/** @brief Returns non-zero if the value at @p n is a boolean. */
#define lua_isboolean(L, n) (lua_type(L, (n)) == LUA_TBOOLEAN)
/** @brief Returns non-zero if the value at @p n is a vector. */
#define lua_isvector(L, n) (lua_type(L, (n)) == LUA_TVECTOR)
/** @brief Returns non-zero if the value at @p n is a coroutine thread. */
#define lua_isthread(L, n) (lua_type(L, (n)) == LUA_TTHREAD)
/** @brief Returns non-zero if the value at @p n is a buffer (Luau extension). */
#define lua_isbuffer(L, n) (lua_type(L, (n)) == LUA_TBUFFER)
/** @brief Returns non-zero if the index @p n is invalid (out of stack range). */
#define lua_isnone(L, n) (lua_type(L, (n)) == LUA_TNONE)
/** @brief Returns non-zero if the value at @p n is nil or the index is invalid. */
#define lua_isnoneornil(L, n) (lua_type(L, (n)) <= LUA_TNIL)

/** @brief Pushes a string literal efficiently using `sizeof` to avoid strlen.  `[-0, +1, m]` */
#define lua_pushliteral(L, s) lua_pushlstring(L, "" s, (sizeof(s) / sizeof(char)) - 1)
/** @brief Pushes a plain C function (no upvalues, no continuation).  `[-0, +1, m]` */
#define lua_pushcfunction(L, fn, debugname) lua_pushcclosurek(L, fn, debugname, 0, NULL)
/** @brief Pushes a C closure with @p nup upvalues and no continuation.  `[-nup, +1, m]` */
#define lua_pushcclosure(L, fn, debugname, nup) lua_pushcclosurek(L, fn, debugname, nup, NULL)
/** @brief Pushes a light userdata with tag 0.  `[-0, +1, -]` */
#define lua_pushlightuserdata(L, p) lua_pushlightuserdatatagged(L, p, 0)

/** @brief Raw table get using a light userdata pointer (tag 0) as key.  `[-0, +1, -]` */
#define lua_rawgetp(L, idx, p) lua_rawgetptagged(L, idx, p, 0)
/** @brief Raw table set using a light userdata pointer (tag 0) as key.  `[-1, +0, m]` */
#define lua_rawsetp(L, idx, p) lua_rawsetptagged(L, idx, p, 0)

/** @brief Sets the global variable named @p s to the value at the top of the stack.  `[-1, +0, e]` */
#define lua_setglobal(L, s) lua_setfield(L, LUA_GLOBALSINDEX, (s))
/** @brief Pushes the global variable named @p s onto the stack.  `[-0, +1, e]` */
#define lua_getglobal(L, s) lua_getfield(L, LUA_GLOBALSINDEX, (s))

/** @brief Converts the value at index @p i to a C string (no length).  Returns NULL if not a string/number. */
#define lua_tostring(L, i) lua_tolstring(L, (i), NULL)

/** @brief Formats and pushes a string, forwarding variadic args to lua_pushfstringL().  `[-0, +1, m]` */
#define lua_pushfstring(L, fmt, ...) lua_pushfstringL(L, fmt, ##__VA_ARGS__)

/*
** {======================================================================
** Debug API
** =======================================================================
*/

typedef struct lua_Debug lua_Debug; // activation record

// Functions to be called by the debugger in specific events
/** @brief Hook function type called at debug events (breakpoints, single-step, etc.). */
typedef void (*lua_Hook)(lua_State* L, lua_Debug* ar);

/** @brief Returns the current call-stack depth of @p L.
 *
 *  `[-0, +0, -]`
 *
 *  @param L  The Lua state.
 *  @return   Number of stack frames. */
LUA_API int lua_stackdepth(lua_State* L);

/** @brief Fills a lua_Debug record for the activation record at @p level.
 *
 *  `[-0, +0|1, e]`
 *
 *  @p level 0 is the currently-running function; 1 is the caller, etc.
 *  @p what is a string of option characters (subset of Lua 5.x):
 *  - `'n'` — fills `name`
 *  - `'s'` — fills `what`, `source`, `short_src`, `linedefined`
 *  - `'l'` — fills `currentline`
 *  - `'u'` — fills `nupvals`
 *  - `'a'` — fills `nparams`, `isvararg`
 *  - `'f'` — pushes the function onto the stack
 *
 *  @param L    The Lua state.
 *  @param level  Call-stack level (0 = current).
 *  @param what   String of option characters.
 *  @param ar     Output activation record to fill.
 *  @return       Non-zero on success, 0 if @p level is out of range. */
LUA_API int lua_getinfo(lua_State* L, int level, const char* what, lua_Debug* ar);

/** @brief Pushes the value of local variable @p n at call-stack @p level.
 *
 *  `[-0, +1|0, -]`
 *
 *  @param L      The Lua state.
 *  @param level  Call-stack level (0 = current function).
 *  @param n      1-based local variable index.
 *  @return       Non-zero if the argument was pushed, 0 if out of range. */
LUA_API int lua_getargument(lua_State* L, int level, int n);

/** @brief Returns the name and pushes the value of local @p n at @p level.
 *
 *  `[-0, +1|0, -]`
 *
 *  @param L      The Lua state.
 *  @param level  Call-stack level.
 *  @param n      1-based local variable index.
 *  @return       Name of the local, or NULL if out of range. */
LUA_API const char* lua_getlocal(lua_State* L, int level, int n);

/** @brief Sets local @p n at @p level to the value at the top of the stack, popping it.
 *
 *  `[-1, +0, -]`
 *
 *  @param L      The Lua state.
 *  @param level  Call-stack level.
 *  @param n      1-based local variable index.
 *  @return       Name of the local, or NULL if out of range (value not popped in that case). */
LUA_API const char* lua_setlocal(lua_State* L, int level, int n);

/** @brief Returns the name and pushes the value of upvalue @p n of the function at @p funcindex.
 *
 *  `[-0, +1|0, -]`
 *
 *  @param L          The Lua state.
 *  @param funcindex  Stack index of the closure.
 *  @param n          1-based upvalue index.
 *  @return           Name of the upvalue, or NULL if out of range. */
LUA_API const char* lua_getupvalue(lua_State* L, int funcindex, int n);

/** @brief Sets upvalue @p n of the function at @p funcindex to the stack top value, popping it.
 *
 *  `[-1, +0, -]`
 *
 *  @param L          The Lua state.
 *  @param funcindex  Stack index of the closure.
 *  @param n          1-based upvalue index.
 *  @return           Name of the upvalue, or NULL if out of range. */
LUA_API const char* lua_setupvalue(lua_State* L, int funcindex, int n);

/** @brief Enables or disables single-step (instruction-level) debugging for @p L.
 *
 *  `[-0, +0, -]`
 *
 *  When enabled the `debugstep` callback in lua_Callbacks fires after every instruction.
 *
 *  @param L        The Lua state.
 *  @param enabled  Non-zero to enable single-step, zero to disable. */
LUA_API void lua_singlestep(lua_State* L, int enabled);

/** @brief Adds or removes a breakpoint at @p line in the function at @p funcindex.
 *
 *  `[-0, +0, -]`
 *
 *  When a breakpoint is hit the VM calls lua_break() internally, which triggers the
 *  `debugbreak` callback.
 *
 *  @param L          The Lua state.
 *  @param funcindex  Stack index of the Lua function.
 *  @param line       Source line number to break on.
 *  @param enabled    Non-zero to set the breakpoint, zero to clear it.
 *  @return           Non-zero if the line is valid for a breakpoint. */
LUA_API int lua_breakpoint(lua_State* L, int funcindex, int line, int enabled);

/** @brief Callback type for lua_getcoverage() — called once per covered source line. */
typedef void (*lua_Coverage)(void* context, const char* function, int linedefined, int depth, const int* hits, size_t size);

/** @brief Visits per-line execution hit counts for the function at @p funcindex.
 *
 *  `[-0, +0, -]`
 *
 *  @param L          The Lua state.
 *  @param funcindex  Stack index of the Lua function.
 *  @param context    Opaque pointer forwarded to @p callback.
 *  @param callback   Called once per function (including nested ones), with the hit-count array. */
LUA_API void lua_getcoverage(lua_State* L, int funcindex, void* context, lua_Coverage callback);

/** @brief Callback called once per function when visiting instruction counters. */
typedef void (*lua_CounterFunction)(void* context, const char* function, int linedefined);
/** @brief Callback called for each counter value within a function. */
typedef void (*lua_CounterValue)(void* context, int kind, int line, uint64_t hits);

// Unlike 'lua_getcoverage', counters are customizable in ways which prevent merging them together
// 'lua_getcounters' will visit the specified function and all nested functions
// 'functionvisit' is called first to establish a function, then multiple calls of 'countervisit' are made for each counter in that function
/** @brief Visits fine-grained instruction counters for the function at @p funcindex.
 *
 *  `[-0, +0, -]`
 *
 *  Unlike lua_getcoverage(), the counter format is configurable and cannot be trivially merged.
 *  For each function (including nested ones), @p functionvisit is called first, followed by
 *  zero or more calls to @p countervisit for each counter in that function.
 *
 *  @param L             The Lua state.
 *  @param funcindex     Stack index of the Lua function.
 *  @param context       Opaque pointer forwarded to both callbacks.
 *  @param functionvisit Called once per function to identify it.
 *  @param countervisit  Called for each (kind, line, hits) counter entry. */
LUA_API void lua_getcounters(lua_State* L, int funcindex, void* context, lua_CounterFunction functionvisit, lua_CounterValue countervisit);

/** @brief Returns a human-readable stack trace string for @p L.
 *
 *  `[-0, +0, -]`
 *
 *  @warning Not thread-safe: stores the result in a shared global array.  Use only for
 *           debugging in single-threaded contexts.
 *
 *  @param L  The Lua state.
 *  @return   Pointer to a static string containing the stack trace. */
// Warning: this function is not thread-safe since it stores the result in a shared global array! Only use for debugging.
LUA_API const char* lua_debugtrace(lua_State* L);

/** @brief Debug activation record filled by lua_getinfo().
 *
 *  Fields are populated depending on the `what` string passed to lua_getinfo(). */
struct lua_Debug
{
    const char* name;      ///< (n) Name of the function (if available).
    const char* what;      ///< (s) Type of function: `"Lua"`, `"C"`, `"main"`, or `"tail"`.
    const char* source;    ///< (s) Source of the chunk (e.g. `"@filename"` or `"=stdin"`).
    const char* short_src; ///< (s) Shortened source suitable for error messages; points into `ssbuf`.
    int linedefined;       ///< (s) Line where the function was defined; -1 for C functions.
    int currentline;       ///< (l) Current executing line; -1 if not available.
    unsigned char nupvals; ///< (u) Number of upvalues.
    unsigned char nparams; ///< (a) Number of fixed parameters.
    char isvararg;         ///< (a) Non-zero if the function is variadic.
    void* userdata;        ///< Only valid inside a luau_callhook callback.

    char ssbuf[LUA_IDSIZE]; ///< Internal buffer used for `short_src`.
};

// }======================================================================

/* Callbacks that can be used to reconfigure behavior of the VM dynamically.
 * These are shared between all coroutines.
 *
 * Note: interrupt is safe to set from an arbitrary thread but all other callbacks
 * can only be changed when the VM is not running any code */
/** @brief VM behaviour callbacks — shared across all coroutines of a state.
 *
 *  Obtain the struct via lua_callbacks().  Most fields must only be written when
 *  no Lua code is executing; the `interrupt` field is the sole exception and may
 *  be set from any thread at any time. */
struct lua_Callbacks
{
    void* userdata; ///< Arbitrary host pointer; never read or written by the VM.

    void (*interrupt)(lua_State* L, int gc);  ///< Called at safepoints (loop back-edges, calls, returns, GC) when non-NULL.  Safe to set from any thread.
    void (*panic)(lua_State* L, int errcode); ///< Called when an unprotected error propagates to the top level (only when longjmp is used).

    void (*userthread)(lua_State* LP, lua_State* L); ///< Called when coroutine @p L is created (LP = parent) or destroyed (LP = NULL).
    int16_t (*useratom)(lua_State* L, const char* s, size_t l); ///< Called when a new string is interned; return a non-negative atom id or -1 to skip.

    void (*debugbreak)(lua_State* L, lua_Debug* ar);     ///< Called when the BREAK instruction is executed (see lua_breakpoint).
    void (*debugstep)(lua_State* L, lua_Debug* ar);      ///< Called after each instruction in single-step mode (see lua_singlestep).
    void (*debuginterrupt)(lua_State* L, lua_Debug* ar); ///< Called when a thread's execution is interrupted by a breakpoint in another thread.
    void (*debugprotectederror)(lua_State* L);           ///< Called when a protected call (lua_pcall) results in an error.

    void (*onallocate)(lua_State* L, size_t osize, size_t nsize); ///< Called on every allocation/reallocation/free; useful for memory profiling.
};
typedef struct lua_Callbacks lua_Callbacks;

/** @brief Returns a pointer to the shared lua_Callbacks struct for this Lua state.
 *
 *  `[-0, +0, -]`
 *
 *  The returned struct is shared among all coroutines.  Most callback fields may only be
 *  changed when no Lua code is running; the `interrupt` field may be set at any time.
 *
 *  @param L  Any coroutine of the target Lua state.
 *  @return   Pointer to the mutable lua_Callbacks struct. */
LUA_API lua_Callbacks* lua_callbacks(lua_State* L);

/******************************************************************************
 * Copyright (c) 2019-2023 Roblox Corporation
 * Copyright (C) 1994-2008 Lua.org, PUC-Rio.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ******************************************************************************/
