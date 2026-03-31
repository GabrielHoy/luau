// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lualib.h"

#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "lapi.h"
#include "lgc.h"
#include "lnumutils.h"

#include <string.h>

LUAU_FASTFLAG(LuauStacklessPcall)

// convert a stack index to positive
#define abs_index(L, i) ((i) > 0 || (i) <= LUA_REGISTRYINDEX ? (i) : lua_gettop(L) + (i) + 1)

/*
** {======================================================
** Error-report functions
** =======================================================
*/

static const char* currfuncname(lua_State* L)
{
    Closure* cl = L->ci > L->base_ci ? curr_func(L) : NULL;
    const char* debugname = cl && cl->isC ? cl->c.debugname + 0 : NULL;

    if (debugname && strcmp(debugname, "__namecall") == 0)
        return L->namecall ? getstr(L->namecall) : NULL;
    else
        return debugname;
}

/** @brief Raises a formatted "invalid argument #N" error for the calling function.
 *
 *  Automatically includes the current C function's debug name in the message
 *  when available (e.g. "invalid argument #1 to 'myfunc' (foo)").  Throws via
 *  `luaL_error` — this function never returns.
 *
 *  @param L         The Lua state.
 *  @param narg      1-based argument index that is invalid.
 *  @param extramsg  Detail message appended in parentheses. */
l_noret luaL_argerrorL(lua_State* L, int narg, const char* extramsg)
{
    const char* fname = currfuncname(L);

    if (fname)
        luaL_error(L, "invalid argument #%d to '%s' (%s)", narg, fname, extramsg);
    else
        luaL_error(L, "invalid argument #%d (%s)", narg, extramsg);
}

/** @brief Raises a "wrong type" error for argument N, naming the expected type.
 *
 *  Produces a message of the form
 *  "invalid argument #N to 'fname' (TYPE expected, got TYPE)" or, if the
 *  argument is entirely absent, "missing argument #N to 'fname' (TYPE expected)".
 *  Throws via `luaL_error` — never returns.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param tname Expected type name (e.g. "string", "table"). */
l_noret luaL_typeerrorL(lua_State* L, int narg, const char* tname)
{
    const char* fname = currfuncname(L);
    const TValue* obj = luaA_toobject(L, narg);

    if (obj)
    {
        if (fname)
            luaL_error(L, "invalid argument #%d to '%s' (%s expected, got %s)", narg, fname, tname, luaT_objtypename(L, obj));
        else
            luaL_error(L, "invalid argument #%d (%s expected, got %s)", narg, tname, luaT_objtypename(L, obj));
    }
    else
    {
        if (fname)
            luaL_error(L, "missing argument #%d to '%s' (%s expected)", narg, fname, tname);
        else
            luaL_error(L, "missing argument #%d (%s expected)", narg, tname);
    }
}

static l_noret tag_error(lua_State* L, int narg, int tag)
{
    luaL_typeerrorL(L, narg, lua_typename(L, tag));
}

/** @brief Pushes a "source:line: " location prefix string onto the stack.
 *
 *  Inspects the call stack at the given `level` and pushes a formatted
 *  location string if source info is available, otherwise pushes an empty
 *  string.  Used internally by `luaL_errorL` to prefix error messages.
 *
 *  @note Can be called without pre-reserving extra stack space.
 *
 *  @param L      The Lua state.
 *  @param level  Call-stack level (1 = current function, 2 = caller, etc.).
 *  Stack: [...] → [..., "source:line: "] */
// Can be called without stack space reservation
void luaL_where(lua_State* L, int level)
{
    lua_Debug ar;
    if (lua_getinfo(L, level, "sl", &ar) && ar.currentline > 0)
    {
        lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
        return;
    }

    lua_rawcheckstack(L, 1);
    lua_pushliteral(L, ""); // else, no information available...
}

/** @brief Formats and throws a Lua error with an automatic "source:line: " prefix.
 *
 *  Calls `luaL_where(L, 1)` to push location info, then `lua_pushvfstring`
 *  to format the message, concatenates both, and throws via `lua_error`.
 *  Never returns.
 *
 *  @note Can be called without pre-reserving extra stack space.
 *
 *  @param L    The Lua state.
 *  @param fmt  printf-style format string.
 *  @param ...  Format arguments. */
// Can be called without stack space reservation
l_noret luaL_errorL(lua_State* L, const char* fmt, ...)
{
    va_list argp;
    va_start(argp, fmt);
    luaL_where(L, 1);
    lua_pushvfstring(L, fmt, argp);
    va_end(argp);
    lua_concat(L, 2);
    lua_error(L);
}

// }======================================================

/** @brief Validates a string argument against a null-terminated list of options.
 *
 *  Reads argument `narg` as a string (or uses `def` if it is nil/absent),
 *  then searches `lst` for a matching entry.  Returns the zero-based index
 *  of the match.  Throws `luaL_argerrorL` if no match is found.
 *
 *  Useful for enum-style string arguments (e.g. "left"/"center"/"right").
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default string when narg is nil/absent (may be NULL to require).
 *  @param lst   Null-terminated array of valid option strings.
 *  @return      Zero-based index of the matched option in `lst`. */
int luaL_checkoption(lua_State* L, int narg, const char* def, const char* const lst[])
{
    const char* name = (def) ? luaL_optstring(L, narg, def) : luaL_checkstring(L, narg);
    int i;
    for (i = 0; lst[i]; i++)
        if (strcmp(lst[i], name) == 0)
            return i;
    const char* msg = lua_pushfstring(L, "invalid option '%s'", name);
    luaL_argerrorL(L, narg, msg);
}

/** @brief Creates or retrieves a named metatable in the registry.
 *
 *  Checks `registry[tname]`.  If it already exists, leaves it on the stack
 *  and returns 0 (the existing metatable is reused).  If not, creates a new
 *  empty table, stores it as `registry[tname]`, pushes it, and returns 1.
 *
 *  This is the standard way to register a C type's metatable so it can be
 *  retrieved later by `luaL_checkudata`.
 *
 *  @param L      The Lua state.
 *  @param tname  Registry key name (typically a unique type identifier string).
 *  @return       1 if a new metatable was created, 0 if it already existed.
 *  Stack: [...] → [..., metatable] */
int luaL_newmetatable(lua_State* L, const char* tname)
{
    lua_getfield(L, LUA_REGISTRYINDEX, tname); // get registry.name
    if (!lua_isnil(L, -1))                     // name already in use?
        return 0;                              // leave previous value on top, but return 0
    lua_pop(L, 1);
    lua_newtable(L); // create metatable
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, tname); // registry.name = metatable
    return 1;
}

/** @brief Validates that argument `ud` is a userdata of the named type.
 *
 *  Retrieves the userdata at stack index `ud`, checks that it has a metatable,
 *  and that its metatable equals `registry[tname]`.  If all checks pass,
 *  returns the raw data pointer.  Throws `luaL_typeerrorL` on failure.
 *
 *  This is the standard type-safety check for C-bound object types.
 *
 *  @param L      The Lua state.
 *  @param ud     Stack index of the userdata argument.
 *  @param tname  Expected type name (same string passed to luaL_newmetatable).
 *  @return       Raw `void*` data pointer of the validated userdata. */
void* luaL_checkudata(lua_State* L, int ud, const char* tname)
{
    void* p = lua_touserdata(L, ud);
    if (p != NULL)
    { // value is a userdata?
        if (lua_getmetatable(L, ud))
        {                                              // does it have a metatable?
            lua_getfield(L, LUA_REGISTRYINDEX, tname); // get correct metatable
            if (lua_rawequal(L, -1, -2))
            {                  // does it have the correct mt?
                lua_pop(L, 2); // remove both metatables
                return p;
            }
        }
    }
    luaL_typeerrorL(L, ud, tname); // else error
}

/** @brief Validates that argument `narg` is a Luau buffer; returns its data pointer.
 *
 *  Calls `lua_tobuffer`.  If the value is not a buffer, throws a type error.
 *  If `len` is non-NULL it receives the buffer's byte length.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param len   Out-param for buffer length in bytes (may be NULL).
 *  @return      Raw `void*` pointer to the buffer's data. */
void* luaL_checkbuffer(lua_State* L, int narg, size_t* len)
{
    void* b = lua_tobuffer(L, narg, len);
    if (!b)
        tag_error(L, narg, LUA_TBUFFER);
    return b;
}

/** @brief Ensures the stack has at least `space` free slots; throws on overflow.
 *
 *  Wrapper around `lua_checkstack` that converts a failure (return 0) into a
 *  Lua error rather than a silent failure.  Use this inside C functions that
 *  know exactly how many slots they need.
 *
 *  @param L      The Lua state.
 *  @param space  Number of additional stack slots required.
 *  @param mes    Message suffix appended to "stack overflow (...)" on error. */
void luaL_checkstack(lua_State* L, int space, const char* mes)
{
    if (!lua_checkstack(L, space))
        luaL_error(L, "stack overflow (%s)", mes);
}

/** @brief Throws a type error if argument `narg` is not of type `t`.
 *
 *  Compares `lua_type(L, narg)` to the LUA_T* constant `t`.  If they differ,
 *  calls `tag_error` which calls `luaL_typeerrorL`.  Does nothing on success.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param t     Expected LUA_T* type constant (e.g. LUA_TTABLE, LUA_TSTRING). */
void luaL_checktype(lua_State* L, int narg, int t)
{
    if (lua_type(L, narg) != t)
        tag_error(L, narg, t);
}

/** @brief Throws an error if argument `narg` is entirely absent (LUA_TNONE).
 *
 *  Does NOT reject nil — only catches the case where the argument was not
 *  passed at all.  Use `luaL_checktype` if you also need to reject nil.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index. */
void luaL_checkany(lua_State* L, int narg)
{
    if (lua_type(L, narg) == LUA_TNONE)
        luaL_error(L, "missing argument #%d", narg);
}

/** @brief Returns the string at argument `narg`; throws a type error if absent.
 *
 *  Calls `lua_tolstring`.  If the result is NULL (not a string or coercible
 *  number), throws `luaL_typeerrorL`.  If `len` is non-NULL it receives the
 *  string's byte length (safe for embedded NUL bytes).
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param len   Out-param for string length (may be NULL).
 *  @return      Pointer to the interned string data (valid until GC). */
const char* luaL_checklstring(lua_State* L, int narg, size_t* len)
{
    const char* s = lua_tolstring(L, narg, len);
    if (!s)
        tag_error(L, narg, LUA_TSTRING);
    return s;
}

/** @brief Like `luaL_checklstring` but returns `def` when argument is nil/absent.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default C string returned when the argument is nil or missing.
 *  @param len   Out-param for string length (may be NULL).
 *  @return      Pointer to the string data, or `def`. */
const char* luaL_optlstring(lua_State* L, int narg, const char* def, size_t* len)
{
    if (lua_isnoneornil(L, narg))
    {
        if (len)
            *len = (def ? strlen(def) : 0);
        return def;
    }
    else
        return luaL_checklstring(L, narg, len);
}

/** @brief Returns the number at argument `narg`; throws a type error if absent.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @return      The number as a double. */
double luaL_checknumber(lua_State* L, int narg)
{
    int isnum;
    double d = lua_tonumberx(L, narg, &isnum);
    if (!isnum)
        tag_error(L, narg, LUA_TNUMBER);
    return d;
}

/** @brief Like `luaL_checknumber` but returns `def` when argument is nil/absent.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default value.
 *  @return      The number as a double, or `def`. */
double luaL_optnumber(lua_State* L, int narg, double def)
{
    return luaL_opt(L, luaL_checknumber, narg, def);
}

/** @brief Returns the boolean at argument `narg`; requires a strict boolean type.
 *
 *  Unlike `lua_toboolean`, this rejects non-boolean truthy values (numbers,
 *  strings, tables).  Throws a type error if the argument is not exactly a
 *  boolean.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @return      0 or 1. */
int luaL_checkboolean(lua_State* L, int narg)
{
    // This checks specifically for boolean values, ignoring
    // all other truthy/falsy values. If the desired result
    // is true if value is present then lua_toboolean should
    // directly be used instead.
    if (!lua_isboolean(L, narg))
        tag_error(L, narg, LUA_TBOOLEAN);
    return lua_toboolean(L, narg);
}

/** @brief Like `luaL_checkboolean` but returns `def` when argument is nil/absent.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default value (0 or 1).
 *  @return      0 or 1, or `def`. */
int luaL_optboolean(lua_State* L, int narg, int def)
{
    return luaL_opt(L, luaL_checkboolean, narg, def);
}

/** @brief Returns the integer at argument `narg` (truncates fractional part).
 *
 *  Internally calls `lua_tointegerx`.  Throws a type error if the value is
 *  not a number.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @return      Value truncated to int. */
int luaL_checkinteger(lua_State* L, int narg)
{
    int isnum;
    int d = lua_tointegerx(L, narg, &isnum);
    if (!isnum)
        tag_error(L, narg, LUA_TNUMBER);
    return d;
}

/** @brief Like `luaL_checkinteger` but returns `def` when argument is nil/absent.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default int value.
 *  @return      Integer value, or `def`. */
int luaL_optinteger(lua_State* L, int narg, int def)
{
    return luaL_opt(L, luaL_checkinteger, narg, def);
}

/** @brief Returns the unsigned integer at argument `narg`.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @return      Value cast to unsigned. */
unsigned luaL_checkunsigned(lua_State* L, int narg)
{
    int isnum;
    unsigned d = lua_tounsignedx(L, narg, &isnum);
    if (!isnum)
        tag_error(L, narg, LUA_TNUMBER);
    return d;
}

/** @brief Like `luaL_checkunsigned` but returns `def` when argument is nil/absent.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default unsigned value.
 *  @return      Unsigned value, or `def`. */
unsigned luaL_optunsigned(lua_State* L, int narg, unsigned def)
{
    return luaL_opt(L, luaL_checkunsigned, narg, def);
}

/** @brief Returns the vector at argument `narg`; throws a type error if absent.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @return      Pointer to the float[3] (or float[4] when LUA_VECTOR_SIZE==4) data. */
const float* luaL_checkvector(lua_State* L, int narg)
{
    const float* v = lua_tovector(L, narg);
    if (!v)
        tag_error(L, narg, LUA_TVECTOR);
    return v;
}

/** @brief Like `luaL_checkvector` but returns `def` when argument is nil/absent.
 *
 *  @param L     The Lua state.
 *  @param narg  1-based argument index.
 *  @param def   Default vector pointer returned when arg is nil/absent.
 *  @return      float* to vector data, or `def`. */
const float* luaL_optvector(lua_State* L, int narg, const float* def)
{
    return luaL_opt(L, luaL_checkvector, narg, def);
}

/** @brief Pushes the named metamethod field from the value at `obj` (if any).
 *
 *  Retrieves the metatable of the value at stack index `obj`, then looks up
 *  `event` in it using a raw get.  If found and non-nil, pushes the field and
 *  returns 1 (the metatable is popped).  If the object has no metatable or
 *  the field is nil, pushes nothing and returns 0.
 *
 *  @param L      The Lua state.
 *  @param obj    Stack index of the object whose metatable to inspect.
 *  @param event  Metamethod name (e.g. "__index", "__tostring").
 *  @return       1 if the field was found and pushed, 0 otherwise.
 *  Stack (success): [...] → [..., metamethod_value]
 *  Stack (failure): [...] → [...] */
int luaL_getmetafield(lua_State* L, int obj, const char* event)
{
    if (!lua_getmetatable(L, obj)) // no metatable?
        return 0;
    lua_pushstring(L, event);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 2); // remove metatable and metafield
        return 0;
    }
    else
    {
        lua_remove(L, -2); // remove only metatable
        return 1;
    }
}

/** @brief Calls the named metamethod on `obj` with the object as the argument.
 *
 *  Equivalent to: if `obj` has metamethod `event`, calls `event(obj)` and
 *  pushes the result.  Returns 1 on success, 0 if no such metamethod exists.
 *
 *  @param L      The Lua state.
 *  @param obj    Stack index of the object.
 *  @param event  Metamethod name (e.g. "__tostring").
 *  @return       1 if metamethod was called and result pushed, 0 otherwise.
 *  Stack (success): [...] → [..., result] */
int luaL_callmeta(lua_State* L, int obj, const char* event)
{
    obj = abs_index(L, obj);
    if (!luaL_getmetafield(L, obj, event)) // no metafield?
        return 0;
    lua_pushvalue(L, obj);
    lua_call(L, 1, 1);
    return 1;
}

static int libsize(const luaL_Reg* l)
{
    int size = 0;
    for (; l->name; l++)
        size++;
    return size;
}

/** @brief Registers an array of C functions into a named global library table.
 *
 *  If `libname` is non-NULL, finds or creates a table at `_G[libname]` (also
 *  stored in `registry._LOADED[libname]` for caching).  Iterates the `l`
 *  array (terminated by {NULL,NULL}) and calls `lua_pushcfunction` +
 *  `lua_setfield` for each entry, registering each function into the table
 *  currently on top of the stack.
 *
 *  @note This is how standard Luau libraries register themselves.  In an
 *  embedding context, use this to expose your own C API modules.
 *
 *  @param L        The Lua state.
 *  @param libname  Name for the globals table entry (NULL to register into
 *                  the table already on top of the stack).
 *  @param l        Null-terminated array of {name, lua_CFunction} pairs. */
void luaL_register(lua_State* L, const char* libname, const luaL_Reg* l)
{
    if (libname)
    {
        int size = libsize(l);
        // check whether lib already exists
        luaL_findtable(L, LUA_REGISTRYINDEX, "_LOADED", 1);
        lua_getfield(L, -1, libname); // get _LOADED[libname]
        if (!lua_istable(L, -1))
        {                  // not found?
            lua_pop(L, 1); // remove previous result
            // try global variable (and create one if it does not exist)
            if (luaL_findtable(L, LUA_GLOBALSINDEX, libname, size) != NULL)
                luaL_error(L, "name conflict for module '%s'", libname);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, libname); // _LOADED[libname] = new table
        }
        lua_remove(L, -2); // remove _LOADED table
    }
    for (; l->name; l++)
    {
        lua_pushcfunction(L, l->func, l->name);
        lua_setfield(L, -2, l->name);
    }
}

/** @brief Traverses a dotted field path, creating intermediate tables as needed.
 *
 *  Starting from the table at `idx`, walks the dotted path in `fname`
 *  (e.g. "a.b.c"), creating any missing sub-tables along the way.  On
 *  success, the final table is left on top of the stack and NULL is returned.
 *  If a non-table value blocks the path, that path component is returned as
 *  a C string (error indicator).
 *
 *  @param L       The Lua state.
 *  @param idx     Stack index of the root table to start from.
 *  @param fname   Dotted field path string (e.g. "awesome.util.table").
 *  @param szhint  Initial capacity hint for any newly created tables.
 *  @return        NULL on success; pointer into `fname` at the blocking
 *                 component on failure.
 *  Stack: [...] → [..., result_table] */
const char* luaL_findtable(lua_State* L, int idx, const char* fname, int szhint)
{
    const char* e;
    lua_pushvalue(L, idx);
    do
    {
        e = strchr(fname, '.');
        if (e == NULL)
            e = fname + strlen(fname);
        lua_pushlstring(L, fname, e - fname);
        lua_rawget(L, -2);
        if (lua_isnil(L, -1))
        {                                                    // no such field?
            lua_pop(L, 1);                                   // remove this nil
            lua_createtable(L, 0, (*e == '.' ? 1 : szhint)); // new table for field
            lua_pushlstring(L, fname, e - fname);
            lua_pushvalue(L, -2);
            lua_settable(L, -4); // set new table into field
        }
        else if (!lua_istable(L, -1))
        {                  // field has a non-table value?
            lua_pop(L, 2); // remove table and value
            return fname;  // return problematic part of the name
        }
        lua_remove(L, -2); // remove previous table
        fname = e + 1;
    } while (*e == '.');
    return NULL;
}

/** @brief Returns the human-readable type name of the value at stack index `idx`.
 *
 *  Like `lua_typename(L, lua_type(L, idx))` but also handles userdata types
 *  that have a registered name (via `__type` metamethod).  Returns "no value"
 *  for an invalid index.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     C string type name (static lifetime; do not free). */
const char* luaL_typename(lua_State* L, int idx)
{
    const TValue* obj = luaA_toobject(L, idx);
    return obj ? luaT_objtypename(L, obj) : "no value";
}

/** @brief Calls a function from within a continuation-aware C function, handling yields.
 *
 *  Must be called from a C function that was registered with a continuation
 *  (`lua_pushcclosurek` with a non-NULL `cont`).  Calls the function on the
 *  stack normally; if the callee yields, propagates the yield state so the VM
 *  can resume via the continuation later.  Returns the continuation's result
 *  on normal completion.
 *
 *  @param L        The Lua state.
 *  @param nargs    Number of arguments on the stack (same semantics as lua_call).
 *  @param nresults Expected number of results (same semantics as lua_call).
 *  @return         Result of the continuation function on normal return, or a
 *                  yield-propagation marker. */
int luaL_callyieldable(lua_State* L, int nargs, int nresults)
{
    api_check(L, iscfunction(L->ci->func));
    Closure* cl = clvalue(L->ci->func);
    api_check(L, cl->c.cont);

    lua_call(L, nargs, nresults);

    if (FFlag::LuauStacklessPcall)
    {
        // yielding means we need to propagate yield; resume will call continuation function later
        if (isyielded(L))
            return C_CALL_YIELD;
    }
    else
    {
        if (L->status == LUA_YIELD || L->status == LUA_BREAK)
            return -1; // -1 is a marker for yielding from C
    }

    return cl->c.cont(L, LUA_OK);
}

/** @brief Builds a human-readable stack traceback string and pushes it.
 *
 *  Walks the call stack of `L1` starting at `level`, skipping pure-C frames,
 *  and formats each Lua frame as "source:line function name\n".  Prepends
 *  `msg` (if non-NULL) followed by a newline.  Pushes the resulting string
 *  onto `L` (which may differ from `L1` — useful for error handlers in a
 *  different coroutine).
 *
 *  @param L      The Lua state to push the result onto.
 *  @param L1     The Lua state whose call stack to walk.
 *  @param msg    Optional prefix message (may be NULL).
 *  @param level  Starting call-stack level (0 = include C frames, 1 = skip current).
 *  Stack (on L): [...] → [..., traceback_string] */
void luaL_traceback(lua_State* L, lua_State* L1, const char* msg, int level)
{
    api_check(L, level >= 0);

    luaL_Strbuf buf;
    luaL_buffinit(L, &buf);

    if (msg)
    {
        luaL_addstring(&buf, msg);
        luaL_addstring(&buf, "\n");
    }

    lua_Debug ar;
    for (int i = level; lua_getinfo(L1, i, "sln", &ar); ++i)
    {
        if (strcmp(ar.what, "C") == 0)
            continue;

        if (ar.source)
            luaL_addstring(&buf, ar.short_src);

        if (ar.currentline > 0)
        {
            char line[32]; // manual conversion for performance
            char* lineend = line + sizeof(line);
            char* lineptr = lineend;
            for (unsigned int r = ar.currentline; r > 0; r /= 10)
                *--lineptr = '0' + (r % 10);

            luaL_addchar(&buf, ':');
            luaL_addlstring(&buf, lineptr, lineend - lineptr);
        }

        if (ar.name)
        {
            luaL_addstring(&buf, " function ");
            luaL_addstring(&buf, ar.name);
        }

        luaL_addchar(&buf, '\n');
    }

    luaL_pushresult(&buf);
}


/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

static size_t getnextbuffersize(lua_State* L, size_t currentsize, size_t desiredsize)
{
    size_t newsize = currentsize + currentsize / 2;

    // check for size overflow
    if (SIZE_MAX - desiredsize < currentsize)
        luaL_error(L, "buffer too large");

    // growth factor might not be enough to satisfy the desired size
    if (newsize < desiredsize)
        newsize = desiredsize;

    return newsize;
}

static char* extendstrbuf(luaL_Strbuf* B, size_t additionalsize, int boxloc)
{
    lua_State* L = B->L;

    if (B->storage)
        LUAU_ASSERT(B->storage == tsvalue(L->top + boxloc));

    char* base = B->storage ? B->storage->data : B->buffer;

    size_t capacity = B->end - base;
    size_t nextsize = getnextbuffersize(B->L, capacity, capacity + additionalsize);

    TString* newStorage = luaS_bufstart(L, nextsize);

    memcpy(newStorage->data, base, B->p - base);

    // place the string storage at the expected position in the stack
    if (base == B->buffer)
    {
        lua_pushnil(L);
        lua_insert(L, boxloc);
    }

    setsvalue(L, L->top + boxloc, newStorage);
    B->p = newStorage->data + (B->p - base);
    B->end = newStorage->data + nextsize;
    B->storage = newStorage;

    return B->p;
}

/** @brief Initialises a string buffer for incremental string construction.
 *
 *  Sets up `B` to use its inline `buffer[LUA_BUFFERSIZE]` array initially.
 *  The buffer grows onto the heap (via a GC-managed TString) when that runs
 *  out.  Must be called before any other `luaL_add*` or `luaL_prep*` calls.
 *
 *  @param L  The Lua state (kept in B->L for GC interaction).
 *  @param B  The string buffer to initialise. */
void luaL_buffinit(lua_State* L, luaL_Strbuf* B)
{
    // start with an internal buffer
    B->p = B->buffer;
    B->end = B->p + LUA_BUFFERSIZE;

    B->L = L;
    B->storage = nullptr;
}

/** @brief Initialises a buffer and immediately reserves `size` bytes.
 *
 *  Calls `luaL_buffinit` then `luaL_prepbuffsize(B, size)`.  Returns a
 *  writable pointer to the reserved region.  Use `luaL_pushresultsize` to
 *  finalise after writing.
 *
 *  @param L     The Lua state.
 *  @param B     The string buffer to initialise.
 *  @param size  Number of bytes to pre-reserve.
 *  @return      Writable pointer to the start of the reserved region. */
char* luaL_buffinitsize(lua_State* L, luaL_Strbuf* B, size_t size)
{
    luaL_buffinit(L, B);
    return luaL_prepbuffsize(B, size);
}

/** @brief Ensures `B` has at least `size` writable bytes available.
 *
 *  If the current free space is insufficient, the underlying storage is
 *  grown (allocating a GC-managed TString on the Lua stack).  Returns a
 *  pointer to the first available writable byte.  You must advance `B->p`
 *  manually (or use `luaL_pushresultsize`) after writing.
 *
 *  @param B     The string buffer.
 *  @param size  Minimum number of bytes required.
 *  @return      Pointer to writable region of at least `size` bytes. */
char* luaL_prepbuffsize(luaL_Strbuf* B, size_t size)
{
    if (size_t(B->end - B->p) < size)
        return extendstrbuf(B, size - (B->end - B->p), -1);
    return B->p;
}

/** @brief Appends `len` bytes from `s` to the buffer.
 *
 *  Grows the buffer if necessary.  Safe for strings containing embedded NUL
 *  bytes.
 *
 *  @param B    The string buffer.
 *  @param s    Source data pointer.
 *  @param len  Number of bytes to copy. */
void luaL_addlstring(luaL_Strbuf* B, const char* s, size_t len)
{
    if (size_t(B->end - B->p) < len)
        extendstrbuf(B, len - (B->end - B->p), -1);

    memcpy(B->p, s, len);
    B->p += len;
}

/** @brief Pops the top string value from the stack and appends it to the buffer.
 *
 *  The top of stack must be a string (or a value coercible to string via
 *  `lua_tolstring`).  The value is consumed (popped) after being appended.
 *  Grows the buffer if necessary.
 *
 *  @param B  The string buffer (B->L must be the active Lua state).
 *  Stack: [..., string] → [...] */
void luaL_addvalue(luaL_Strbuf* B)
{
    lua_State* L = B->L;

    size_t vl;
    if (const char* s = lua_tolstring(L, -1, &vl))
    {
        if (size_t(B->end - B->p) < vl)
            extendstrbuf(B, vl - (B->end - B->p), -2);

        memcpy(B->p, s, vl);
        B->p += vl;

        lua_pop(L, 1);
    }
}

/** @brief Appends any Lua value at stack index `idx` to the buffer as its string form.
 *
 *  Handles nil, booleans, numbers, strings, and vectors directly.  For other
 *  types, falls back to `luaL_tolstring` (respects `__tostring`).  Does NOT
 *  consume the value from the stack (unlike `luaL_addvalue`).
 *
 *  @param B    The string buffer.
 *  @param idx  Stack index of the value to stringify and append. */
void luaL_addvalueany(luaL_Strbuf* B, int idx)
{
    lua_State* L = B->L;

    switch (lua_type(L, idx))
    {
    case LUA_TNONE:
    {
        LUAU_ASSERT(!"expected value");
        break;
    }
    case LUA_TNIL:
        luaL_addstring(B, "nil");
        break;
    case LUA_TBOOLEAN:
        if (lua_toboolean(L, idx))
            luaL_addstring(B, "true");
        else
            luaL_addstring(B, "false");
        break;
    case LUA_TNUMBER:
    {
        double n = lua_tonumber(L, idx);
        char s[LUAI_MAXNUM2STR];
        char* e = luai_num2str(s, n);
        luaL_addlstring(B, s, e - s);
        break;
    }
    case LUA_TSTRING:
    {
        size_t len;
        const char* s = lua_tolstring(L, idx, &len);
        luaL_addlstring(B, s, len);
        break;
    }
    default:
    {
        size_t len;
        luaL_tolstring(L, idx, &len);

        // note: luaL_addlstring assumes box is stored at top of stack, so we can't call it here
        // instead we use luaL_addvalue which will take the string from the top of the stack and add that
        luaL_addvalue(B);
    }
    }
}

/** @brief Finalises the buffer and pushes the accumulated string onto the stack.
 *
 *  If the buffer used heap storage, converts it to a proper interned Lua
 *  string in-place (zero-copy when possible).  Otherwise copies the inline
 *  buffer into a new string.  The resulting string is on top of the stack.
 *
 *  @param B  The string buffer to finalise.
 *  Stack: [...] → [..., result_string]  (or replaces the storage slot) */
void luaL_pushresult(luaL_Strbuf* B)
{
    lua_State* L = B->L;

    if (TString* storage = B->storage)
    {
        luaC_checkGC(L);

        // if we finished just at the end of the string buffer, we can convert it to a mutable stirng without a copy
        if (B->p == B->end)
        {
            setsvalue(L, L->top - 1, luaS_buffinish(L, storage));
        }
        else
        {
            setsvalue(L, L->top - 1, luaS_newlstr(L, storage->data, B->p - storage->data));
        }
    }
    else
    {
        lua_pushlstring(L, B->buffer, B->p - B->buffer);
    }
}

/** @brief Advances the write pointer by `size` bytes, then finalises the buffer.
 *
 *  Convenience wrapper: after a `luaL_prepbuffsize` / direct-write sequence,
 *  call this to commit `size` bytes and push the result string.
 *
 *  @param B     The string buffer.
 *  @param size  Number of bytes written since the last `luaL_prepbuffsize` call.
 *  Stack: [...] → [..., result_string] */
void luaL_pushresultsize(luaL_Strbuf* B, size_t size)
{
    B->p += size;
    luaL_pushresult(B);
}

// }======================================================

/** @brief Converts the value at `idx` to a string and pushes it.
 *
 *  Checks for a `__tostring` metamethod first (via `luaL_callmeta`); if
 *  found, calls it and uses the result.  Otherwise formats the value directly:
 *  nil → "nil", boolean → "true"/"false", number → formatted double,
 *  vector → "x, y, z[, w]", string → pushed as-is, other → "TYPE: 0xADDR".
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the value to convert.
 *  @param len  Out-param for result string length (may be NULL).
 *  @return     Pointer to the pushed string data.
 *  Stack: [...] → [..., string_representation] */
const char* luaL_tolstring(lua_State* L, int idx, size_t* len)
{
    if (luaL_callmeta(L, idx, "__tostring")) // is there a metafield?
    {
        const char* s = lua_tolstring(L, -1, len);
        if (!s)
            luaL_error(L, "'__tostring' must return a string");
        return s;
    }

    switch (lua_type(L, idx))
    {
    case LUA_TNIL:
        lua_pushliteral(L, "nil");
        break;
    case LUA_TBOOLEAN:
        lua_pushstring(L, (lua_toboolean(L, idx) ? "true" : "false"));
        break;
    case LUA_TNUMBER:
    {
        double n = lua_tonumber(L, idx);
        char s[LUAI_MAXNUM2STR];
        char* e = luai_num2str(s, n);
        lua_pushlstring(L, s, e - s);
        break;
    }
    case LUA_TVECTOR:
    {
        const float* v = lua_tovector(L, idx);

        char s[LUAI_MAXNUM2STR * LUA_VECTOR_SIZE];
        char* e = s;
        for (int i = 0; i < LUA_VECTOR_SIZE; ++i)
        {
            if (i != 0)
            {
                *e++ = ',';
                *e++ = ' ';
            }
            e = luai_num2str(e, v[i]);
        }
        lua_pushlstring(L, s, e - s);
        break;
    }
    case LUA_TSTRING:
        lua_pushvalue(L, idx);
        break;
    default:
    {
        const void* ptr = lua_topointer(L, idx);
        unsigned long long enc = lua_encodepointer(L, uintptr_t(ptr));
        lua_pushfstring(L, "%s: 0x%016llx", luaL_typename(L, idx), enc);
        break;
    }
    }
    return lua_tolstring(L, -1, len);
}
