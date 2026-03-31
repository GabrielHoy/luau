// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lualib.h"

#include <stdlib.h>

static const luaL_Reg lualibs[] = {
    {"", luaopen_base},
    {LUA_COLIBNAME, luaopen_coroutine},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_OSLIBNAME, luaopen_os},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_DBLIBNAME, luaopen_debug},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {LUA_BITLIBNAME, luaopen_bit32},
    {LUA_BUFFERLIBNAME, luaopen_buffer},
    {LUA_VECLIBNAME, luaopen_vector},
    {NULL, NULL},
};

/** @brief Opens all standard Luau libraries into the global environment.
 *
 *  Iterates the built-in `lualibs` registration table and calls each
 *  `luaopen_*` function via `lua_call`, passing the library name string as
 *  its argument.  After this call all standard globals are available:
 *  `math`, `string`, `table`, `os`, `coroutine`, `debug`, `utf8`, `bit32`,
 *  `buffer`, `vector`.
 *
 *  @note Luau intentionally omits `io`, `package`, and `require` — you must
 *  inject those yourself (e.g. push a custom require via lua_pushcfunction).
 *
 *  @param L  The Lua state. */
void luaL_openlibs(lua_State* L)
{
    const luaL_Reg* lib = lualibs;
    for (; lib->func; lib++)
    {
        lua_pushcfunction(L, lib->func, NULL);
        lua_pushstring(L, lib->name);
        lua_call(L, 1, 0);
    }
}

/** @brief Locks down all standard library tables to prevent script modification.
 *
 *  Iterates the global table and calls `lua_setreadonly(true)` on every table
 *  value found there.  Also marks the built-in string metatable and the
 *  globals table itself as read-only, then sets `safeenv = true` on globals
 *  so the VM can fast-path certain built-in operations.
 *
 *  Typical pattern: call `luaL_openlibs` then `luaL_sandbox` on the main
 *  state, then `luaL_sandboxthread` on each new coroutine created for
 *  untrusted script execution.
 *
 *  @param L  The main Lua state (not a sandboxed thread). */
void luaL_sandbox(lua_State* L)
{
    // set all libraries to read-only
    lua_pushnil(L);
    while (lua_next(L, LUA_GLOBALSINDEX) != 0)
    {
        if (lua_istable(L, -1))
            lua_setreadonly(L, -1, true);

        lua_pop(L, 1);
    }

    // set all builtin metatables to read-only
    lua_pushliteral(L, "");
    if (lua_getmetatable(L, -1))
    {
        lua_setreadonly(L, -1, true);
        lua_pop(L, 2);
    }
    else
    {
        lua_pop(L, 1);
    }

    // set globals to readonly and activate safeenv since the env is immutable
    lua_setreadonly(L, LUA_GLOBALSINDEX, true);
    lua_setsafeenv(L, LUA_GLOBALSINDEX, true);
}

/** @brief Gives a thread its own writable globals table that proxies reads to
 *         the (read-only) shared globals.
 *
 *  Creates a new table and attaches a metatable whose `__index` field points
 *  at the current global table.  That metatable is itself marked read-only.
 *  The new table is then installed as this thread's globals via
 *  `lua_replace(L, LUA_GLOBALSINDEX)`.
 *
 *  Result: script code in this coroutine can define its own globals freely
 *  without polluting the shared state, while reads for undefined names fall
 *  through to the sandboxed standard library.
 *
 *  @note Must call `luaL_sandbox` on the main state first.
 *  @note If the same thread loads code twice, reset safeenv to false between loads.
 *
 *  @param L  The coroutine thread to sandbox (typically freshly created with
 *            lua_newthread). */
void luaL_sandboxthread(lua_State* L)
{
    // create new global table that proxies reads to original table
    lua_newtable(L);

    lua_newtable(L);
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_setfield(L, -2, "__index");
    lua_setreadonly(L, -1, true);

    lua_setmetatable(L, -2);

    // we can set safeenv now although it's important to set it to false if code is loaded twice into the thread
    lua_replace(L, LUA_GLOBALSINDEX);
    lua_setsafeenv(L, LUA_GLOBALSINDEX, true);
}

static void* l_alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0)
    {
        free(ptr);
        return NULL;
    }
    else
        return realloc(ptr, nsize);
}

/** @brief Creates a new Luau VM state using the default system allocator.
 *
 *  Wraps `lua_newstate` with a trivial `malloc`/`realloc`/`free` allocator.
 *  For production embeddings you may want to supply your own allocator via
 *  `lua_newstate` directly (e.g. for memory budgets or tracking).
 *
 *  Typical usage:
 *  @code
 *  lua_State* L = luaL_newstate();
 *  luaL_openlibs(L);
 *  // compile + load bytecode, then lua_pcall ...
 *  lua_close(L);
 *  @endcode
 *
 *  @return  A newly allocated lua_State*, or NULL if the initial allocation
 *           failed. */
lua_State* luaL_newstate(void)
{
    return lua_newstate(l_alloc, NULL);
}
