// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lapi.h"

#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lfunc.h"
#include "lgc.h"
#include "ldo.h"
#include "ludata.h"
#include "lvm.h"
#include "lnumutils.h"
#include "lbuffer.h"

#include <string.h>

/*
 * This file contains most implementations of core Lua APIs from lua.h.
 *
 * These implementations should use api_check macros to verify that stack and type contracts hold; it's the callers
 * responsibility to, for example, pass a valid table index to lua_rawgetfield. Generally errors should only be raised
 * for conditions caller can't predict such as an out-of-memory error.
 *
 * The caller is expected to handle stack reservation (by using less than LUA_MINSTACK slots or by calling lua_checkstack).
 * To ensure this is handled correctly, use api_incr_top(L) when pushing values to the stack.
 *
 * Functions that push any collectable objects to the stack *should* call luaC_threadbarrier. Failure to do this can result
 * in stack references that point to dead objects since black threads don't get rescanned.
 *
 * Functions that push newly created objects to the stack *should* call luaC_checkGC in addition to luaC_threadbarrier.
 * Failure to do this can result in OOM since GC may never run.
 *
 * Note that luaC_checkGC may mark the thread and paint it black; functions that call both before pushing objects must
 * therefore call luaC_checkGC before luaC_threadbarrier to guarantee the object is pushed to a gray thread.
 */

const char* lua_ident = "$Lua: Lua 5.1.4 Copyright (C) 1994-2008 Lua.org, PUC-Rio $\n"
                        "$Authors: R. Ierusalimschy, L. H. de Figueiredo & W. Celes $\n"
                        "$URL: www.lua.org $\n";

const char* luau_ident = "$Luau: Copyright (C) 2019-2024 Roblox Corporation $\n"
                         "$URL: luau.org $\n";

#define api_checknelems(L, n) api_check(L, (n) <= (L->top - L->base))

#define api_checkvalidindex(L, i) api_check(L, (i) != luaO_nilobject)

#define api_incr_top(L) \
    { \
        api_check(L, L->top < L->ci->top); \
        L->top++; \
    }

#define api_update_top(L, p) \
    { \
        api_check(L, p >= L->base && p <= L->ci->top); \
        L->top = p; \
    }

#define updateatom(L, ts) \
    { \
        if (ts->atom == ATOM_UNDEF) \
            ts->atom = L->global->cb.useratom ? L->global->cb.useratom(L, ts->data, ts->len) : -1; \
    }

static LuaTable* getcurrenv(lua_State* L)
{
    if (L->ci == L->base_ci) // no enclosing function?
        return L->gt;        // use global table as environment
    else
        return curr_func(L)->env;
}

static LUAU_NOINLINE TValue* pseudo2addr(lua_State* L, int idx)
{
    api_check(L, lua_ispseudo(idx));
    switch (idx)
    { // pseudo-indices
    case LUA_REGISTRYINDEX:
        return registry(L);
    case LUA_ENVIRONINDEX:
    {
        sethvalue(L, &L->global->pseudotemp, getcurrenv(L));
        return &L->global->pseudotemp;
    }
    case LUA_GLOBALSINDEX:
    {
        sethvalue(L, &L->global->pseudotemp, L->gt);
        return &L->global->pseudotemp;
    }
    default:
    {
        Closure* func = curr_func(L);
        idx = LUA_GLOBALSINDEX - idx;
        return (idx <= func->nupvalues) ? &func->c.upvals[idx - 1] : cast_to(TValue*, luaO_nilobject);
    }
    }
}

static LUAU_FORCEINLINE TValue* index2addr(lua_State* L, int idx)
{
    if (idx > 0)
    {
        TValue* o = L->base + (idx - 1);
        api_check(L, idx <= L->ci->top - L->base);
        if (o >= L->top)
            return cast_to(TValue*, luaO_nilobject);
        else
            return o;
    }
    else if (idx > LUA_REGISTRYINDEX)
    {
        api_check(L, idx != 0 && -idx <= L->top - L->base);
        return L->top + idx;
    }
    else
    {
        return pseudo2addr(L, idx);
    }
}

/** @brief Internal helper — converts a stack index to a raw TValue pointer.
 *
 *  Returns NULL (not luaO_nilobject) when the index is out of range or points
 *  to an unset slot.  Used internally by the auxiliary library and debugger.
 *
 *  @param L    The Lua state.
 *  @param idx  Any valid stack index (positive, negative, or pseudo-index).
 *  @return     Pointer to the TValue at that index, or NULL. */
const TValue* luaA_toobject(lua_State* L, int idx)
{
    StkId p = index2addr(L, idx);
    return (p == luaO_nilobject) ? NULL : p;
}

/** @brief Internal helper — pushes an arbitrary TValue onto the stack.
 *
 *  Used by the debugger and auxiliary library to push values obtained from
 *  internal VM structures without going through the normal index-based API.
 *
 *  @param L  The Lua state.
 *  @param o  Pointer to a TValue to copy onto the top of the stack.
 *  Stack: [...] → [..., value] */
void luaA_pushobject(lua_State* L, const TValue* o)
{
    setobj2s(L, L->top, o);
    api_incr_top(L);
}

/** @brief Ensures the stack has at least `size` additional free slots.
 *
 *  Grows the stack if needed.  Returns 1 on success, 0 if growing is not
 *  possible (would exceed LUAI_MAXCSTACK).  Unlike `lua_rawcheckstack`, this
 *  does NOT throw — the caller must check the return value.
 *
 *  Call this at the start of any C function that pushes a variable or
 *  potentially large number of values (beyond the guaranteed LUA_MINSTACK).
 *
 *  @param L     The Lua state.
 *  @param size  Number of additional stack slots required.
 *  @return      1 if enough space is available (or was allocated), 0 on failure. */
int lua_checkstack(lua_State* L, int size)
{
    int res = 1;
    if (size > LUAI_MAXCSTACK || (L->top - L->base + size) > LUAI_MAXCSTACK)
        res = 0; // stack overflow
    else if (size > 0)
    {
        if (stacklimitreached(L, size))
        {
            struct CallContext
            {
                int size;

                static void run(lua_State* L, void* ud)
                {
                    CallContext* ctx = (CallContext*)ud;

                    luaD_growstack(L, ctx->size);
                }
            } ctx = {size};

            // there could be no memory to extend the stack
            if (luaD_rawrunprotected(L, &CallContext::run, &ctx) != LUA_OK)
                return 0;
        }
        else
        {
            condhardstacktests(luaD_reallocstack(L, L->stacksize - EXTRA_STACK, 0));
        }

        expandstacklimit(L, L->top + size);
    }
    return res;
}

/** @brief Unconditionally grows the stack by `size` slots; panics on OOM.
 *
 *  Unlike `lua_checkstack`, this version does not return a status — it calls
 *  `luaD_checkstack` which throws a Lua error if memory cannot be allocated.
 *  Used in code paths where failure is genuinely unrecoverable.
 *
 *  @param L     The Lua state.
 *  @param size  Number of additional stack slots to guarantee. */
void lua_rawcheckstack(lua_State* L, int size)
{
    luaD_checkstack(L, size);
    expandstacklimit(L, L->top + size);
}

/** @brief Moves `n` values from the top of `from`'s stack to `to`'s stack.
 *
 *  Both states must belong to the same global VM (`from->global == to->global`).
 *  Values are popped from `from` and pushed onto `to` in order (bottom-most
 *  first), preserving their relative order.  If `from == to`, this is a no-op.
 *
 *  @param from  Source Lua state (values are removed from here).
 *  @param to    Destination Lua state (values are pushed here).
 *  @param n     Number of values to move (must be ≤ stack size of `from`).
 *  Stack (from): [..., v1, v2, ..., vN] → [...]
 *  Stack (to):   [...] → [..., v1, v2, ..., vN] */
void lua_xmove(lua_State* from, lua_State* to, int n)
{
    if (from == to)
        return;
    api_checknelems(from, n);
    api_check(from, from->global == to->global);
    api_check(from, to->ci->top - to->top >= n);
    luaC_threadbarrier(to);

    StkId ttop = to->top;
    StkId ftop = from->top - n;
    for (int i = 0; i < n; i++)
        setobj2s(to, ttop + i, ftop + i);

    from->top = ftop;
    to->top = ttop + n;
}

/** @brief Copies (not moves) the value at `from[idx]` onto `to`'s stack.
 *
 *  Unlike `lua_xmove`, the source value is NOT removed from `from`.  Both
 *  states must share the same global VM.
 *
 *  @param from  Source Lua state (value is NOT removed).
 *  @param to    Destination Lua state.
 *  @param idx   Stack index in `from` of the value to copy.
 *  Stack (to): [...] → [..., value] */
void lua_xpush(lua_State* from, lua_State* to, int idx)
{
    api_check(from, from->global == to->global);
    luaC_threadbarrier(to);
    setobj2s(to, to->top, index2addr(from, idx));
    api_incr_top(to);
}

/** @brief Creates a new coroutine (lua_State) sharing this VM's globals.
 *
 *  Allocates a new thread object, pushes it onto `L`'s stack, and fires the
 *  `userthread` callback (if set in `lua_callbacks(L)->userthread`).  The new
 *  thread starts in a "suspended" state with an empty stack; use
 *  `lua_resume` to start it.
 *
 *  @param L  The parent Lua state.
 *  @return   The new coroutine's lua_State* (also pushed onto L's stack).
 *  Stack: [...] → [..., thread] */
lua_State* lua_newthread(lua_State* L)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    lua_State* L1 = luaE_newthread(L);
    setthvalue(L, L->top, L1);
    api_incr_top(L);
    global_State* g = L->global;
    if (g->cb.userthread)
        g->cb.userthread(L, L1);
    return L1;
}

/** @brief Returns the main (root) thread of the VM that owns `L`.
 *
 *  Every Luau VM has exactly one main thread created by `lua_newstate`.
 *  Coroutines created with `lua_newthread` share the same global state but
 *  are NOT the main thread.
 *
 *  @param L  Any Lua state belonging to the VM.
 *  @return   The main lua_State* of the VM. */
lua_State* lua_mainthread(lua_State* L)
{
    return L->global->mainthread;
}

/*
** basic stack manipulation
*/

/** @brief Converts a (potentially negative) stack index to an absolute positive index.
 *
 *  Negative indices are relative to the top of the stack (-1 = top).  This
 *  function converts them to positive indices (1 = base) that remain valid
 *  even after push/pop operations change the stack top.  Pseudo-indices
 *  (LUA_REGISTRYINDEX etc.) are returned unchanged.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index (positive, negative, or pseudo-index).
 *  @return     Equivalent positive (absolute) stack index. */
int lua_absindex(lua_State* L, int idx)
{
    api_check(L, (idx > 0 && idx <= L->top - L->base) || (idx < 0 && -idx <= L->top - L->base) || lua_ispseudo(idx));
    return idx > 0 || lua_ispseudo(idx) ? idx : cast_int(L->top - L->base) + idx + 1;
}

/** @brief Returns the number of values currently on the stack (= index of the top).
 *
 *  An empty stack returns 0.  This value equals the highest valid positive
 *  stack index: slot 1 is the bottom, slot `lua_gettop(L)` is the top.
 *  Equivalent to: `top - base` in the internal stack layout.
 *
 *  @param L  The Lua state.
 *  @return   Number of values on the stack (0 if empty). */
int lua_gettop(lua_State* L)
{
    return cast_int(L->top - L->base);
}

/** @brief Sets the stack top to the given index, growing or shrinking it.
 *
 *  - Positive `idx`: sets the top to that absolute position; new slots are
 *    filled with nil if growing, excess values are discarded if shrinking.
 *  - Negative `idx`: shrinks the stack by `|idx|-1` slots (e.g. -1 = no-op,
 *    -2 = pop one value).
 *  - `lua_settop(L, 0)` clears the entire stack.
 *
 *  `lua_pop(L, n)` is defined as `lua_settop(L, -(n)-1)`.
 *
 *  @param L    The Lua state.
 *  @param idx  New stack top: positive absolute index, or negative relative index. */
void lua_settop(lua_State* L, int idx)
{
    if (idx >= 0)
    {
        api_check(L, idx <= L->stack_last - L->base);
        while (L->top < L->base + idx)
            setnilvalue(L->top++);
        L->top = L->base + idx;
    }
    else
    {
        api_check(L, -(idx + 1) <= (L->top - L->base));
        L->top += idx + 1; // `subtract' index (index is negative)
    }
}

/** @brief Removes the element at stack index `idx`, shifting values down.
 *
 *  All values above `idx` shift down one position to close the gap.
 *  The stack shrinks by one.
 *
 *  @param L    The Lua state.
 *  @param idx  Index of the element to remove (must not be a pseudo-index).
 *  Stack: [..., idx_val, a, b] → [..., a, b]  (idx_val removed) */
void lua_remove(lua_State* L, int idx)
{
    StkId p = index2addr(L, idx);
    api_checkvalidindex(L, p);
    while (++p < L->top)
        setobj2s(L, p - 1, p);
    L->top--;
}

/** @brief Moves the top value to position `idx`, shifting values up.
 *
 *  The top value is removed from the top and inserted at `idx`.  All values
 *  previously at `idx` and above shift up one position.  Stack size is unchanged.
 *
 *  Common use: after pushing a key for `lua_rawset(L, LUA_REGISTRYINDEX)` when
 *  the table is already below, use `lua_insert` to reorder key and table.
 *
 *  @param L    The Lua state.
 *  @param idx  Destination index for the top value (must not be a pseudo-index).
 *  Stack: [..., a, b, top_val] → [..., top_val, a, b]  (idx points to where a was) */
void lua_insert(lua_State* L, int idx)
{
    luaC_threadbarrier(L);
    StkId p = index2addr(L, idx);
    api_checkvalidindex(L, p);
    for (StkId q = L->top; q > p; q--)
        setobj2s(L, q, q - 1);
    setobj2s(L, p, L->top);
}

/** @brief Pops the top value and stores it at index `idx` (no shifting).
 *
 *  The value at `idx` is overwritten with the top value, and the top is
 *  popped.  Stack shrinks by one.  Unlike `lua_insert`, no other slots move.
 *
 *  Special cases: `LUA_ENVIRONINDEX` replaces the current function's
 *  environment; `LUA_GLOBALSINDEX` replaces the global table.
 *
 *  @param L    The Lua state.
 *  @param idx  Destination index (may be a pseudo-index).
 *  Stack: [..., new_val] → [...]  (idx slot now holds new_val) */
void lua_replace(lua_State* L, int idx)
{
    api_checknelems(L, 1);
    luaC_threadbarrier(L);
    StkId o = index2addr(L, idx);
    api_checkvalidindex(L, o);
    if (idx == LUA_ENVIRONINDEX)
    {
        api_check(L, L->ci != L->base_ci);
        Closure* func = curr_func(L);
        api_check(L, ttistable(L->top - 1));
        func->env = hvalue(L->top - 1);
        luaC_barrier(L, func, L->top - 1);
    }
    else if (idx == LUA_GLOBALSINDEX)
    {
        api_check(L, ttistable(L->top - 1));
        L->gt = hvalue(L->top - 1);
    }
    else
    {
        setobj(L, o, L->top - 1);
        if (idx < LUA_GLOBALSINDEX) // function upvalue?
            luaC_barrier(L, curr_func(L), L->top - 1);
    }
    L->top--;
}

/** @brief Duplicates the value at `idx` and pushes the copy onto the top.
 *
 *  The original value is not disturbed.  Stack grows by one.  Very commonly
 *  used when you need to leave a value in place AND also use it as an
 *  argument to another call.
 *
 *  @param L    The Lua state.
 *  @param idx  Index of the value to duplicate.
 *  Stack: [..., val] → [..., val, val] */
void lua_pushvalue(lua_State* L, int idx)
{
    luaC_threadbarrier(L);
    StkId o = index2addr(L, idx);
    setobj2s(L, L->top, o);
    api_incr_top(L);
}

/*
** access functions (stack -> C)
*/

/** @brief Returns the type tag of the value at stack index `idx`.
 *
 *  Returns one of the LUA_T* constants: LUA_TNIL, LUA_TBOOLEAN,
 *  LUA_TNUMBER, LUA_TSTRING, LUA_TTABLE, LUA_TFUNCTION, LUA_TUSERDATA,
 *  LUA_TTHREAD, LUA_TLIGHTUSERDATA, LUA_TVECTOR, LUA_TBUFFER.
 *  Returns LUA_TNONE (-1) for an invalid (out-of-range) index.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     LUA_T* type constant, or LUA_TNONE if index is invalid. */
int lua_type(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    return (o == luaO_nilobject) ? LUA_TNONE : ttype(o);
}

/** @brief Returns the name string for a LUA_T* type constant.
 *
 *  Does not inspect the stack — takes a type tag integer directly.  Returns
 *  "no value" for LUA_TNONE.  The returned string has static lifetime.
 *
 *  @param L  The Lua state (unused but required by the API signature).
 *  @param t  A LUA_T* type constant.
 *  @return   Human-readable type name (e.g. "string", "table", "no value"). */
const char* lua_typename(lua_State* L, int t)
{
    return (t == LUA_TNONE) ? "no value" : luaT_typenames[t];
}

/** @brief Returns 1 if the value at `idx` is a C closure (pushed via lua_pushcfunction).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     1 if a C function/closure, 0 otherwise. */
int lua_iscfunction(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    return iscfunction(o);
}

/** @brief Returns 1 if the value at `idx` is a Luau bytecode function (Lua closure).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     1 if a Lua closure, 0 otherwise. */
int lua_isLfunction(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    return isLfunction(o);
}

/** @brief Returns 1 if the value at `idx` can be converted to a number.
 *
 *  Returns 1 for actual numbers AND for strings that represent valid numbers
 *  (coercion).  Use `lua_type(L, idx) == LUA_TNUMBER` if you want strict
 *  type checking without coercion.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     1 if number or coercible string, 0 otherwise. */
int lua_isnumber(lua_State* L, int idx)
{
    TValue n;
    const TValue* o = index2addr(L, idx);
    return tonumber(o, &n);
}

/** @brief Returns 1 if the value at `idx` is a string OR a number (both produce strings).
 *
 *  Numbers can be coerced to strings in Lua, so this returns 1 for both types.
 *  Use `lua_type(L, idx) == LUA_TSTRING` for strict string-only checking.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     1 if string or number, 0 otherwise. */
int lua_isstring(lua_State* L, int idx)
{
    int t = lua_type(L, idx);
    return (t == LUA_TSTRING || t == LUA_TNUMBER);
}

/** @brief Returns 1 if the value at `idx` is full userdata OR light userdata.
 *
 *  Use `lua_type(L, idx) == LUA_TUSERDATA` if you need to distinguish between
 *  the two kinds (full = GC-managed block; light = raw unmanaged pointer).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     1 if full or light userdata, 0 otherwise. */
int lua_isuserdata(lua_State* L, int idx)
{
    const TValue* o = index2addr(L, idx);
    return (ttisuserdata(o) || ttislightuserdata(o));
}

/** @brief Compares two values for equality WITHOUT invoking `__eq` metamethods.
 *
 *  Primitive equality: two values are equal only if they have the same type
 *  and value.  Tables and userdata compare by identity (pointer equality).
 *  Returns 0 if either index is invalid.
 *
 *  @param L       The Lua state.
 *  @param index1  Stack index of the first value.
 *  @param index2  Stack index of the second value.
 *  @return        1 if values are primitively equal, 0 otherwise. */
int lua_rawequal(lua_State* L, int index1, int index2)
{
    StkId o1 = index2addr(L, index1);
    StkId o2 = index2addr(L, index2);
    return (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0 : luaO_rawequalObj(o1, o2);
}

/** @brief Compares two values for equality, invoking `__eq` if applicable.
 *
 *  Uses full Lua equality semantics including metamethod dispatch.  Can call
 *  back into Lua code.  Returns 0 if either index is invalid.
 *
 *  @param L       The Lua state.
 *  @param index1  Stack index of the first value.
 *  @param index2  Stack index of the second value.
 *  @return        1 if values are equal (by Lua rules), 0 otherwise. */
int lua_equal(lua_State* L, int index1, int index2)
{
    StkId o1, o2;
    int i;
    o1 = index2addr(L, index1);
    o2 = index2addr(L, index2);
    i = (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0 : equalobj(L, o1, o2);
    return i;
}

/** @brief Returns 1 if `index1 < index2` using Lua's `<` operator semantics.
 *
 *  Dispatches through `__lt` metamethods when applicable.  Returns 0 if
 *  either index is invalid.
 *
 *  @param L       The Lua state.
 *  @param index1  Stack index of the left-hand value.
 *  @param index2  Stack index of the right-hand value.
 *  @return        1 if LHS < RHS, 0 otherwise. */
int lua_lessthan(lua_State* L, int index1, int index2)
{
    StkId o1, o2;
    int i;
    o1 = index2addr(L, index1);
    o2 = index2addr(L, index2);
    i = (o1 == luaO_nilobject || o2 == luaO_nilobject) ? 0 : luaV_lessthan(L, o1, o2);
    return i;
}

/** @brief Converts the value at `idx` to a double; reports success via `*isnum`.
 *
 *  Attempts numeric coercion for strings.  Sets `*isnum = 1` on success,
 *  `*isnum = 0` and returns 0.0 on failure.  `isnum` may be NULL.
 *
 *  @param L      The Lua state.
 *  @param idx    Stack index.
 *  @param isnum  Out-param set to 1 on success, 0 on failure (may be NULL).
 *  @return       The number as a double, or 0.0 on failure. */
double lua_tonumberx(lua_State* L, int idx, int* isnum)
{
    TValue n;
    const TValue* o = index2addr(L, idx);
    if (tonumber(o, &n))
    {
        if (isnum)
            *isnum = 1;
        return nvalue(o);
    }
    else
    {
        if (isnum)
            *isnum = 0;
        return 0;
    }
}

/** @brief Converts the value at `idx` to an int (truncates fractional part).
 *
 *  Attempts numeric coercion for strings.  Sets `*isnum` and returns 0 on
 *  failure.  `isnum` may be NULL.
 *
 *  @param L      The Lua state.
 *  @param idx    Stack index.
 *  @param isnum  Out-param (may be NULL).
 *  @return       Truncated integer value, or 0 on failure. */
int lua_tointegerx(lua_State* L, int idx, int* isnum)
{
    TValue n;
    const TValue* o = index2addr(L, idx);
    if (tonumber(o, &n))
    {
        int res;
        double num = nvalue(o);
        luai_num2int(res, num);
        if (isnum)
            *isnum = 1;
        return res;
    }
    else
    {
        if (isnum)
            *isnum = 0;
        return 0;
    }
}

/** @brief Converts the value at `idx` to an unsigned integer.
 *
 *  Same semantics as `lua_tointegerx` but returns unsigned.
 *
 *  @param L      The Lua state.
 *  @param idx    Stack index.
 *  @param isnum  Out-param (may be NULL).
 *  @return       Unsigned integer value, or 0 on failure. */
unsigned lua_tounsignedx(lua_State* L, int idx, int* isnum)
{
    TValue n;
    const TValue* o = index2addr(L, idx);
    if (tonumber(o, &n))
    {
        unsigned res;
        double num = nvalue(o);
        luai_num2unsigned(res, num);
        if (isnum)
            *isnum = 1;
        return res;
    }
    else
    {
        if (isnum)
            *isnum = 0;
        return 0;
    }
}

/** @brief Returns 1 if the value at `idx` is truthy (anything except nil and false).
 *
 *  This is NOT a strict boolean check — numbers, strings, tables, etc. all
 *  return 1.  Use `lua_isboolean` + `lua_toboolean` if you need strict boolean.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     0 if nil or false, 1 for everything else. */
int lua_toboolean(lua_State* L, int idx)
{
    const TValue* o = index2addr(L, idx);
    return !l_isfalse(o);
}

/** @brief Converts the value at `idx` to a C string; coerces numbers.
 *
 *  If the value is a number, it is converted to a string in-place on the
 *  stack (modifying the slot).  Returns NULL if conversion is not possible.
 *  The returned pointer is valid only until the value is popped or the string
 *  is otherwise GC'd — do not store it across Lua API calls.
 *
 *  @note If `len` is non-NULL, it receives the byte length (safe for embedded NULs).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @param len  Out-param for string length (may be NULL).
 *  @return     Pointer to the interned string bytes, or NULL on failure. */
const char* lua_tolstring(lua_State* L, int idx, size_t* len)
{
    StkId o = index2addr(L, idx);
    if (!ttisstring(o))
    {
        luaC_threadbarrier(L);
        if (!luaV_tostring(L, o))
        { // conversion failed?
            if (len != NULL)
                *len = 0;
            return NULL;
        }
        luaC_checkGC(L);
        o = index2addr(L, idx); // previous call may reallocate the stack
    }
    if (len != NULL)
        *len = tsvalue(o)->len;
    return svalue(o);
}

/** @brief Returns the string data at `idx` and its Luau "atom" (interned integer ID).
 *
 *  Atoms are small integers assigned by the `useratom` callback when a string
 *  is first interned.  They allow O(1) string comparison against known symbols.
 *  Returns NULL if the value is not a string (no coercion).  Sets `*atom = -1`
 *  if no `useratom` callback is configured.
 *
 *  @param L     The Lua state.
 *  @param idx   Stack index.
 *  @param atom  Out-param for the atom integer (may be NULL).
 *  @return      Pointer to the NUL-terminated string data, or NULL. */
const char* lua_tostringatom(lua_State* L, int idx, int* atom)
{
    StkId o = index2addr(L, idx);
    if (!ttisstring(o))
        return NULL;
    TString* s = tsvalue(o);
    if (atom)
    {
        updateatom(L, s);
        *atom = s->atom;
    }
    return getstr(s);
}

/** @brief Like `lua_tostringatom` but also returns the byte length via `*len`.
 *
 *  Combines the functionality of `lua_tolstring` and `lua_tostringatom`.
 *  Safe for strings with embedded NUL bytes.
 *
 *  @param L     The Lua state.
 *  @param idx   Stack index.
 *  @param len   Out-param for byte length (may be NULL).
 *  @param atom  Out-param for atom integer (may be NULL).
 *  @return      Pointer to string data, or NULL if not a string. */
const char* lua_tolstringatom(lua_State* L, int idx, size_t* len, int* atom)
{
    StkId o = index2addr(L, idx);

    if (!ttisstring(o))
    {
        if (len)
            *len = 0;
        return NULL;
    }

    TString* s = tsvalue(o);
    if (len)
        *len = s->len;
    if (atom)
    {
        updateatom(L, s);
        *atom = s->atom;
    }

    return getstr(s);
}

/** @brief Returns the method name string from the most recent `__namecall` dispatch.
 *
 *  When the VM executes a method call `obj:method(...)`, it sets the
 *  `namecall` field on the state before invoking `__namecall`.  This lets the
 *  metamethod dispatcher identify which method was called via a single shared
 *  `__namecall` handler rather than per-method closures.
 *
 *  Returns NULL if not currently inside a `__namecall` invocation.
 *
 *  @param L     The Lua state.
 *  @param atom  Out-param for the atom integer (may be NULL).
 *  @return      Method name string, or NULL if not in a namecall context. */
const char* lua_namecallatom(lua_State* L, int* atom)
{
    TString* s = L->namecall;
    if (!s)
        return NULL;
    if (atom)
    {
        updateatom(L, s);
        *atom = s->atom;
    }
    return getstr(s);
}

/** @brief Returns a pointer to the float components of a vector value at `idx`.
 *
 *  Luau has a native vector type (LUA_TVECTOR).  When `LUA_VECTOR_SIZE == 4`
 *  the array has 4 components (xyzw); otherwise 3 (xyz).
 *  Returns NULL if the value is not a vector.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     Pointer to float[LUA_VECTOR_SIZE], or NULL if not a vector. */
const float* lua_tovector(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    if (!ttisvector(o))
        return NULL;
    return vvalue(o);
}

/** @brief Returns the "length" of the value at `idx` (the `#` operator).
 *
 *  - String: byte length.
 *  - Userdata: allocated byte size.
 *  - Buffer: byte length.
 *  - Table: border (highest integer key n with t[n] != nil), via `luaH_getn`.
 *  - Other types: 0.
 *  Does NOT invoke `__len` metamethods (use the VM's length opcode for that).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     Length in bytes/elements, or 0 for unsupported types. */
int lua_objlen(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    switch (ttype(o))
    {
    case LUA_TSTRING:
        return tsvalue(o)->len;
    case LUA_TUSERDATA:
        return uvalue(o)->len;
    case LUA_TBUFFER:
        return bufvalue(o)->len;
    case LUA_TTABLE:
        return luaH_getn(hvalue(o));
    default:
        return 0;
    }
}

/** @brief Returns the C function pointer from a C closure at `idx`.
 *
 *  Returns NULL if the value is not a C function.  Useful for inspecting or
 *  comparing registered callbacks.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     The lua_CFunction pointer, or NULL. */
lua_CFunction lua_tocfunction(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    return (!iscfunction(o)) ? NULL : cast_to(lua_CFunction, clvalue(o)->c.f);
}

/** @brief Returns the raw pointer from a light userdata at `idx`.
 *
 *  Light userdata is a bare `void*` pushed with `lua_pushlightuserdata`.  It
 *  is NOT GC-managed — the pointer's lifetime is the caller's responsibility.
 *  Returns NULL if the value is not light userdata.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     The raw pointer, or NULL if not light userdata. */
void* lua_tolightuserdata(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    return (!ttislightuserdata(o)) ? NULL : pvalue(o);
}

/** @brief Like `lua_tolightuserdata` but also validates the light userdata tag.
 *
 *  Returns NULL if the value is not light userdata OR if its tag doesn't match.
 *  Tags allow multiple light-userdata "types" to coexist with O(1) distinction.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @param tag  Expected tag (0 to LUA_LUTAG_LIMIT-1).
 *  @return     Raw pointer if type and tag match, NULL otherwise. */
void* lua_tolightuserdatatagged(lua_State* L, int idx, int tag)
{
    StkId o = index2addr(L, idx);
    return (!ttislightuserdata(o) || lightuserdatatag(o) != tag) ? NULL : pvalue(o);
}

/** @brief Returns the data pointer for full userdata or light userdata at `idx`.
 *
 *  - Full userdata (`LUA_TUSERDATA`): returns the embedded data block pointer.
 *    The block is GC-managed; valid until the userdata is collected.
 *  - Light userdata (`LUA_TLIGHTUSERDATA`): returns the raw pointer as-is.
 *  - Anything else: returns NULL.
 *
 *  This is the primary way to retrieve the C struct pointer from an object
 *  created with `lua_newuserdatatagged`.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     Data pointer, or NULL if not userdata. */
void* lua_touserdata(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    if (ttisuserdata(o))
        return uvalue(o)->data;
    else if (ttislightuserdata(o))
        return pvalue(o);
    else
        return NULL;
}

/** @brief Like `lua_touserdata` but only succeeds if the full userdata's tag matches.
 *
 *  Returns NULL if the value is not full userdata or if the tag differs.
 *  Use this for fast type dispatch when you register multiple C types using
 *  different numeric tags via `lua_newuserdatatagged`.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @param tag  Expected userdata tag (0 to LUA_UTAG_LIMIT-1).
 *  @return     Data pointer if tag matches, NULL otherwise. */
void* lua_touserdatatagged(lua_State* L, int idx, int tag)
{
    StkId o = index2addr(L, idx);
    return (ttisuserdata(o) && uvalue(o)->tag == tag) ? uvalue(o)->data : NULL;
}

/** @brief Returns the numeric tag of the full userdata at `idx`.
 *
 *  Returns -1 if the value is not full userdata.  Tags are set at creation
 *  time via `lua_newuserdatatagged` and can be changed with `lua_setuserdatatag`.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     Tag integer (0 to LUA_UTAG_LIMIT-1), or -1. */
int lua_userdatatag(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    if (ttisuserdata(o))
        return uvalue(o)->tag;
    return -1;
}

/** @brief Returns the numeric tag of the light userdata at `idx`.
 *
 *  Returns -1 if the value is not light userdata.  Light userdata tags are
 *  set at push time via `lua_pushlightuserdatatagged`.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     Tag integer (0 to LUA_LUTAG_LIMIT-1), or -1. */
int lua_lightuserdatatag(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    if (ttislightuserdata(o))
        return lightuserdatatag(o);
    return -1;
}

/** @brief Returns the coroutine lua_State* for a thread value at `idx`.
 *
 *  Returns NULL if the value is not a thread.  The returned state is managed
 *  by the GC; do not use it after it has been collected.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     lua_State* of the coroutine, or NULL. */
lua_State* lua_tothread(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    return (!ttisthread(o)) ? NULL : thvalue(o);
}

/** @brief Returns the data pointer for a Luau buffer value at `idx`.
 *
 *  Luau buffers (`LUA_TBUFFER`) are mutable byte arrays with a fixed size.
 *  Returns NULL if the value is not a buffer.  If `len` is non-NULL it
 *  receives the buffer's byte length.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @param len  Out-param for buffer byte length (may be NULL).
 *  @return     Raw `void*` to the buffer data, or NULL. */
void* lua_tobuffer(lua_State* L, int idx, size_t* len)
{
    StkId o = index2addr(L, idx);

    if (!ttisbuffer(o))
        return NULL;

    Buffer* b = bufvalue(o);

    if (len)
        *len = b->len;

    return b->data;
}

/** @brief Returns a unique (but untyped) pointer for any GC-managed value.
 *
 *  For full userdata and light userdata, returns the data pointer.  For
 *  all other collectable types (tables, functions, threads, strings, etc.),
 *  returns the GC object header pointer.  Useful for identity comparison or
 *  as a map key.  Returns NULL for non-collectable primitives (nil, bool, number).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index.
 *  @return     A stable pointer unique to this object, or NULL. */
const void* lua_topointer(lua_State* L, int idx)
{
    StkId o = index2addr(L, idx);
    switch (ttype(o))
    {
    case LUA_TUSERDATA:
        return uvalue(o)->data;
    case LUA_TLIGHTUSERDATA:
        return pvalue(o);
    default:
        return iscollectable(o) ? gcvalue(o) : NULL;
    }
}

/*
** push functions (C -> stack)
*/

/** @brief Pushes a nil value onto the stack.
 *
 *  @param L  The Lua state.
 *  Stack: [...] → [..., nil] */
void lua_pushnil(lua_State* L)
{
    setnilvalue(L->top);
    api_incr_top(L);
}

/** @brief Pushes a double-precision floating-point number onto the stack.
 *
 *  Luau stores all numbers as doubles internally.
 *
 *  @param L  The Lua state.
 *  @param n  The number to push.
 *  Stack: [...] → [..., n] */
void lua_pushnumber(lua_State* L, double n)
{
    setnvalue(L->top, n);
    api_incr_top(L);
}

/** @brief Pushes an integer value onto the stack (stored internally as double).
 *
 *  @param L  The Lua state.
 *  @param n  Integer value to push.
 *  Stack: [...] → [..., n] */
void lua_pushinteger(lua_State* L, int n)
{
    setnvalue(L->top, cast_num(n));
    api_incr_top(L);
}

/** @brief Pushes an unsigned integer value onto the stack (stored as double).
 *
 *  @param L  The Lua state.
 *  @param u  Unsigned value to push.
 *  Stack: [...] → [..., u] */
void lua_pushunsigned(lua_State* L, unsigned u)
{
    setnvalue(L->top, cast_num(u));
    api_incr_top(L);
}

/** @brief Pushes a 4-component vector value (x, y, z, w) onto the stack.
 *
 *  Only available when LUA_VECTOR_SIZE == 4.  The 3-component variant is used
 *  otherwise (see below).  Vectors are a native Luau value type (LUA_TVECTOR),
 *  not tables — they live entirely on the stack with no GC allocation.
 *
 *  @param L  The Lua state.
 *  @param x  X component.
 *  @param y  Y component.
 *  @param z  Z component.
 *  @param w  W component.
 *  Stack: [...] → [..., vector(x,y,z,w)] */
#if LUA_VECTOR_SIZE == 4
void lua_pushvector(lua_State* L, float x, float y, float z, float w)
{
    setvvalue(L->top, x, y, z, w);
    api_incr_top(L);
}
#else
void lua_pushvector(lua_State* L, float x, float y, float z)
{
    setvvalue(L->top, x, y, z, 0.0f);
    api_incr_top(L);
}
#endif

/** @brief Pushes a string of exactly `len` bytes onto the stack.
 *
 *  The string is interned — if an identical string already exists in the VM,
 *  the same object is reused.  Safe for strings with embedded NUL bytes.
 *  May trigger a GC cycle (`luaC_checkGC`).
 *
 *  @param L    The Lua state.
 *  @param s    Pointer to the string data (copied; caller may free immediately after).
 *  @param len  Byte length of the string.
 *  Stack: [...] → [..., string] */
void lua_pushlstring(lua_State* L, const char* s, size_t len)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    setsvalue(L, L->top, luaS_newlstr(L, s, len));
    api_incr_top(L);
}

/** @brief Pushes a NUL-terminated C string onto the stack, or nil if `s` is NULL.
 *
 *  Calls `lua_pushlstring` with `strlen(s)`.  Not safe for strings with
 *  embedded NUL bytes — use `lua_pushlstring` for those.
 *
 *  @param L  The Lua state.
 *  @param s  NUL-terminated string, or NULL (pushes nil).
 *  Stack: [...] → [..., string]  (or [..., nil] if s==NULL) */
void lua_pushstring(lua_State* L, const char* s)
{
    if (s == NULL)
        lua_pushnil(L);
    else
        lua_pushlstring(L, s, strlen(s));
}

/** @brief Formats a string using a va_list and pushes the result onto the stack.
 *
 *  Supports a subset of printf specifiers: `%s`, `%d`, `%f`, `%p`, `%q`, `%%`.
 *  Returns a pointer to the pushed string data.
 *
 *  @param L     The Lua state.
 *  @param fmt   Format string.
 *  @param argp  Argument list (va_list).
 *  @return      Pointer to the formatted string on the stack.
 *  Stack: [...] → [..., formatted_string] */
const char* lua_pushvfstring(lua_State* L, const char* fmt, va_list argp)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    const char* ret = luaO_pushvfstring(L, fmt, argp);
    return ret;
}

/** @brief Formats a string using varargs and pushes the result onto the stack.
 *
 *  Variadic wrapper around `lua_pushvfstring`.  Supports the same limited
 *  format specifier set.  The `L` suffix distinguishes it from the macro
 *  `lua_pushfstring` which is defined in lua.h.
 *
 *  @param L    The Lua state.
 *  @param fmt  Format string.
 *  @param ...  Format arguments.
 *  @return     Pointer to the formatted string on the stack.
 *  Stack: [...] → [..., formatted_string] */
const char* lua_pushfstringL(lua_State* L, const char* fmt, ...)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    va_list argp;
    va_start(argp, fmt);
    const char* ret = luaO_pushvfstring(L, fmt, argp);
    va_end(argp);
    return ret;
}

/** @brief Pushes a C closure with upvalues and an optional yield continuation.
 *
 *  This is the underlying function that `lua_pushcfunction(L, fn, name)` maps
 *  to (in Luau, lua_pushcfunction takes 3 args: state, fn, debugname).
 *
 *  - `nup` values are popped from the stack and become the closure's upvalues
 *    (accessible inside `fn` via upvalue pseudo-indices).
 *  - `cont` is a continuation function called by the VM when a yield happens
 *    inside a pcall that wraps this function.  Pass NULL for non-yieldable fns.
 *  - `debugname` is stored for error messages and the debugger.
 *
 *  @param L          The Lua state.
 *  @param fn         The C function to wrap.
 *  @param debugname  Name shown in tracebacks (may be NULL).
 *  @param nup        Number of upvalues to pop from stack into the closure.
 *  @param cont       Yield continuation (may be NULL).
 *  Stack: [..., up1, ..., upN] → [..., closure] */
void lua_pushcclosurek(lua_State* L, lua_CFunction fn, const char* debugname, int nup, lua_Continuation cont)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    api_checknelems(L, nup);
    Closure* cl = luaF_newCclosure(L, nup, getcurrenv(L));
    cl->c.f = fn;
    cl->c.cont = cont;
    cl->c.debugname = debugname;
    L->top -= nup;
    while (nup--)
        setobj2n(L, &cl->c.upvals[nup], L->top + nup);
    setclvalue(L, L->top, cl);
    LUAU_ASSERT(iswhite(obj2gco(cl)));
    api_incr_top(L);
}

/** @brief Pushes a boolean value onto the stack.
 *
 *  Non-zero `b` is normalised to true (1).
 *
 *  @param L  The Lua state.
 *  @param b  0 for false, any non-zero for true.
 *  Stack: [...] → [..., boolean] */
void lua_pushboolean(lua_State* L, int b)
{
    setbvalue(L->top, (b != 0)); // ensure that true is 1
    api_incr_top(L);
}

/** @brief Pushes a tagged light userdata (raw pointer) onto the stack.
 *
 *  Light userdata is a bare `void*` — not GC-managed, no finalizer, not
 *  collected.  The `tag` lets you distinguish multiple light-userdata "types"
 *  at O(1) cost via `lua_tolightuserdatatagged`.
 *  `lua_pushlightuserdata(L, p)` is the tag-0 shorthand macro.
 *
 *  @param L    The Lua state.
 *  @param p    The raw pointer to push.
 *  @param tag  Tag value (0 to LUA_LUTAG_LIMIT-1).
 *  Stack: [...] → [..., lightuserdata] */
void lua_pushlightuserdatatagged(lua_State* L, void* p, int tag)
{
    api_check(L, unsigned(tag) < LUA_LUTAG_LIMIT);
    setpvalue(L->top, p, tag);
    api_incr_top(L);
}

/** @brief Pushes the current thread (coroutine) `L` itself onto the stack.
 *
 *  Returns 1 if this thread is the main thread of the VM, 0 otherwise.
 *  Useful when you need to pass the current coroutine as a Lua value.
 *
 *  @param L  The Lua state.
 *  @return   1 if L is the main thread, 0 if it is a coroutine.
 *  Stack: [...] → [..., thread] */
int lua_pushthread(lua_State* L)
{
    luaC_threadbarrier(L);
    setthvalue(L, L->top, L);
    api_incr_top(L);
    return L->global->mainthread == L;
}

/*
** get functions (Lua -> stack)
*/

/** @brief Performs a table read with full metamethod dispatch (`__index`).
 *
 *  Pops the key from the top of the stack, looks it up in the table at `idx`
 *  (invoking `__index` if applicable), and pushes the result.  Net stack
 *  change is zero (pop key, push value).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table (or object with __index metamethod).
 *  @return     LUA_T* type tag of the resulting value.
 *  Stack: [..., key] → [..., value] */
int lua_gettable(lua_State* L, int idx)
{
    luaC_threadbarrier(L);
    StkId t = index2addr(L, idx);
    api_checkvalidindex(L, t);
    luaV_gettable(L, t, L->top - 1, L->top - 1);
    return ttype(L->top - 1);
}

/** @brief Pushes `table[k]` onto the stack, invoking `__index` if applicable.
 *
 *  Equivalent to pushing the string key `k` then calling `lua_gettable`, but
 *  without actually pushing the key.  Commonly used for `table.field` reads.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param k    String field name.
 *  @return     LUA_T* type tag of the value.
 *  Stack: [...] → [..., value] */
int lua_getfield(lua_State* L, int idx, const char* k)
{
    luaC_threadbarrier(L);
    StkId t = index2addr(L, idx);
    api_checkvalidindex(L, t);
    TValue key;
    setsvalue(L, &key, luaS_new(L, k));
    luaV_gettable(L, t, &key, L->top);
    api_incr_top(L);
    return ttype(L->top - 1);
}

/** @brief Pushes `table[k]` without invoking `__index` metamethods.
 *
 *  Direct hash lookup — no metamethod dispatch.  Requires the value at `idx`
 *  to be a table (asserts in debug builds).
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param k    String field name.
 *  @return     LUA_T* type tag of the value.
 *  Stack: [...] → [..., value] */
int lua_rawgetfield(lua_State* L, int idx, const char* k)
{
    luaC_threadbarrier(L);
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    TValue key;
    setsvalue(L, &key, luaS_new(L, k));
    setobj2s(L, L->top, luaH_getstr(hvalue(t), tsvalue(&key)));
    api_incr_top(L);
    return ttype(L->top - 1);
}

/** @brief Pops the key and pushes `table[key]` without invoking `__index`.
 *
 *  The key type is arbitrary (unlike `lua_rawgetfield` which requires a string).
 *  Mandatory for registry operations — always use `lua_rawget`/`lua_rawset`
 *  on `LUA_REGISTRYINDEX`, never the metamethod variants.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @return     LUA_T* type tag of the value.
 *  Stack: [..., key] → [..., value] */
int lua_rawget(lua_State* L, int idx)
{
    luaC_threadbarrier(L);
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    setobj2s(L, L->top - 1, luaH_get(hvalue(t), L->top - 1));
    return ttype(L->top - 1);
}

/** @brief Pushes `table[n]` (integer key) without invoking `__index`.
 *
 *  Optimised integer-keyed lookup in the table's array portion.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param n    Integer key.
 *  @return     LUA_T* type tag of the value.
 *  Stack: [...] → [..., value] */
int lua_rawgeti(lua_State* L, int idx, int n)
{
    luaC_threadbarrier(L);
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    setobj2s(L, L->top, luaH_getnum(hvalue(t), n));
    api_incr_top(L);
    return ttype(L->top - 1);
}

/** @brief Pushes `table[p, tag]` using a tagged pointer as the key; no metamethods.
 *
 *  Luau extension: allows using a `(void*, int tag)` pair as a table key,
 *  enabling efficient C-pointer-keyed lookups without going through string hashing.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param p    Pointer key.
 *  @param tag  Tag associated with the pointer key.
 *  @return     LUA_T* type tag of the value.
 *  Stack: [...] → [..., value] */
int lua_rawgetptagged(lua_State* L, int idx, void* p, int tag)
{
    luaC_threadbarrier(L);
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    setobj2s(L, L->top, luaH_getp(hvalue(t), p, tag));
    api_incr_top(L);
    return ttype(L->top - 1);
}

/** @brief Pushes a new empty table with pre-allocated capacity.
 *
 *  Pre-allocating avoids repeated rehashing when you know the approximate
 *  size upfront.  Both hints are advisory — the actual allocation may be
 *  rounded up to a power of two.
 *
 *  @param L       The Lua state.
 *  @param narray  Expected number of sequential integer keys (array part).
 *  @param nrec    Expected number of hash-part keys (string/other keys).
 *  Stack: [...] → [..., table] */
void lua_createtable(lua_State* L, int narray, int nrec)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    sethvalue(L, L->top, luaH_new(L, narray, nrec));
    api_incr_top(L);
}

/** @brief Enables or disables write-protection on a table.
 *
 *  When a table is read-only, any attempt to write to it (including from Lua
 *  code) throws a "attempt to modify a readonly table" error.  Cannot be
 *  applied to the registry itself.  Used by `luaL_sandbox` to freeze standard
 *  library tables.
 *
 *  @param L          The Lua state.
 *  @param objindex   Stack index of the table.
 *  @param enabled    1 to make read-only, 0 to make writable again. */
void lua_setreadonly(lua_State* L, int objindex, int enabled)
{
    const TValue* o = index2addr(L, objindex);
    api_check(L, ttistable(o));
    LuaTable* t = hvalue(o);
    api_check(L, t != hvalue(registry(L)));
    t->readonly = bool(enabled);
}

/** @brief Returns 1 if the table at `objindex` is write-protected, 0 otherwise.
 *
 *  @param L          The Lua state.
 *  @param objindex   Stack index of the table.
 *  @return           1 if read-only, 0 if writable. */
int lua_getreadonly(lua_State* L, int objindex)
{
    const TValue* o = index2addr(L, objindex);
    api_check(L, ttistable(o));
    LuaTable* t = hvalue(o);
    int res = t->readonly;
    return res;
}

/** @brief Marks a table as a "safe environment" for VM fast-path optimisations.
 *
 *  When `safeenv` is true, the VM can assume certain globals (e.g. `math.abs`,
 *  `string.format`) have not been overridden, enabling inlining.  Set to false
 *  if you load code into a sandboxed thread more than once (to clear stale
 *  inline caches).
 *
 *  @param L          The Lua state.
 *  @param objindex   Stack index of the globals table.
 *  @param enabled    1 to mark as safe, 0 to invalidate. */
void lua_setsafeenv(lua_State* L, int objindex, int enabled)
{
    const TValue* o = index2addr(L, objindex);
    api_check(L, ttistable(o));
    LuaTable* t = hvalue(o);
    t->safeenv = bool(enabled);
}

/** @brief Pushes the metatable of the value at `objindex`, if it has one.
 *
 *  - Tables and full userdata: push their per-object metatable.
 *  - Other types: push the type-shared metatable (set via `lua_setmetatable`
 *    with a non-table object index, stored in `L->global->mt[type]`).
 *  Returns 0 and pushes nothing if there is no metatable.
 *
 *  @param L          The Lua state.
 *  @param objindex   Stack index of the value to inspect.
 *  @return           1 if a metatable was found and pushed, 0 otherwise.
 *  Stack (success): [...] → [..., metatable] */
int lua_getmetatable(lua_State* L, int objindex)
{
    luaC_threadbarrier(L);
    LuaTable* mt = NULL;
    const TValue* obj = index2addr(L, objindex);
    switch (ttype(obj))
    {
    case LUA_TTABLE:
        mt = hvalue(obj)->metatable;
        break;
    case LUA_TUSERDATA:
        mt = uvalue(obj)->metatable;
        break;
    default:
        mt = L->global->mt[ttype(obj)];
        break;
    }
    if (mt)
    {
        sethvalue(L, L->top, mt);
        api_incr_top(L);
    }
    return mt != NULL;
}

/** @brief Pushes the environment table of a function or thread.
 *
 *  @warning **Luau restriction**: unlike Lua 5.1, this only works for
 *  `LUA_TFUNCTION` and `LUA_TTHREAD`.  Calling it on userdata pushes nil.
 *  This is why the AwesomeWM shim layer must replace `lua_getfenv` on userdata
 *  with the registry-keyed-by-pointer pattern.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of a function or thread.
 *  Stack: [...] → [..., env_table]  (or [..., nil] for non-function/thread) */
void lua_getfenv(lua_State* L, int idx)
{
    luaC_threadbarrier(L);
    StkId o = index2addr(L, idx);
    api_checkvalidindex(L, o);
    switch (ttype(o))
    {
    case LUA_TFUNCTION:
        sethvalue(L, L->top, clvalue(o)->env);
        break;
    case LUA_TTHREAD:
        sethvalue(L, L->top, thvalue(o)->gt);
        break;
    default:
        setnilvalue(L->top);
        break;
    }
    api_incr_top(L);
}

/*
** set functions (stack -> Lua)
*/

/** @brief Pops key and value and performs a table write with `__newindex` dispatch.
 *
 *  Equivalent to `table[key] = value` in Lua, invoking `__newindex` if set.
 *  Both key and value are popped; stack shrinks by 2.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  Stack: [..., key, value] → [...] */
void lua_settable(lua_State* L, int idx)
{
    api_checknelems(L, 2);
    StkId t = index2addr(L, idx);
    api_checkvalidindex(L, t);
    luaV_settable(L, t, L->top - 2, L->top - 1);
    L->top -= 2; // pop index and value
}

/** @brief Pops the top value and stores it as `table[k]`, invoking `__newindex`.
 *
 *  Equivalent to `table.k = value` in Lua.  The value is popped; stack
 *  shrinks by 1.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param k    String field name.
 *  Stack: [..., value] → [...] */
void lua_setfield(lua_State* L, int idx, const char* k)
{
    api_checknelems(L, 1);
    StkId t = index2addr(L, idx);
    api_checkvalidindex(L, t);
    TValue key;
    setsvalue(L, &key, luaS_new(L, k));
    luaV_settable(L, t, &key, L->top - 1);
    L->top--;
}

/** @brief Pops the top value and stores it as `table[k]` without `__newindex`.
 *
 *  Direct hash write — no metamethod dispatch.  Throws if the table is
 *  read-only.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table (must be a table).
 *  @param k    String field name.
 *  Stack: [..., value] → [...] */
void lua_rawsetfield(lua_State* L, int idx, const char* k)
{
    api_checknelems(L, 1);
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    if (hvalue(t)->readonly)
        luaG_readonlyerror(L);
    setobj2t(L, luaH_setstr(L, hvalue(t), luaS_new(L, k)), L->top - 1);
    luaC_barriert(L, hvalue(t), L->top - 1);
    L->top--;
}

/** @brief Pops key and value and stores `table[key] = value` without metamethods.
 *
 *  The key type is arbitrary.  Always use this (not `lua_settable`) when
 *  writing to `LUA_REGISTRYINDEX`.  Throws if the table is read-only.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  Stack: [..., key, value] → [...] */
void lua_rawset(lua_State* L, int idx)
{
    api_checknelems(L, 2);
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    if (hvalue(t)->readonly)
        luaG_readonlyerror(L);
    setobj2t(L, luaH_set(L, hvalue(t), L->top - 2), L->top - 1);
    luaC_barriert(L, hvalue(t), L->top - 1);
    L->top -= 2;
}

/** @brief Pops the top value and stores it as `table[n]` (integer key) without metamethods.
 *
 *  Optimised array-part write.  Throws if the table is read-only.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param n    Integer key.
 *  Stack: [..., value] → [...] */
void lua_rawseti(lua_State* L, int idx, int n)
{
    api_checknelems(L, 1);
    StkId o = index2addr(L, idx);
    api_check(L, ttistable(o));
    if (hvalue(o)->readonly)
        luaG_readonlyerror(L);
    setobj2t(L, luaH_setnum(L, hvalue(o), n), L->top - 1);
    luaC_barriert(L, hvalue(o), L->top - 1);
    L->top--;
}

/** @brief Pops the top value and stores it as `table[p,tag]` (pointer key) without metamethods.
 *
 *  Luau extension: tagged-pointer table keys for efficient C-struct-to-table
 *  associations without string hashing.  Throws if the table is read-only.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @param p    Pointer key.
 *  @param tag  Tag associated with the pointer key.
 *  Stack: [..., value] → [...] */
void lua_rawsetptagged(lua_State* L, int idx, void* p, int tag)
{
    api_checknelems(L, 1);
    StkId o = index2addr(L, idx);
    api_check(L, ttistable(o));
    if (hvalue(o)->readonly)
        luaG_readonlyerror(L);
    setobj2t(L, luaH_setp(L, hvalue(o), p, tag), L->top - 1);
    luaC_barriert(L, hvalue(o), L->top - 1);
    L->top--;
}

/** @brief Pops a table (or nil) and sets it as the metatable of the value at `objindex`.
 *
 *  - Tables and full userdata: sets their per-object metatable.
 *  - Other types: sets the shared type-level metatable in `L->global->mt[type]`,
 *    affecting ALL values of that type in the VM.
 *  Passing nil clears the metatable.  Throws if the table is read-only.
 *
 *  @param L          The Lua state.
 *  @param objindex   Stack index of the value to modify.
 *  @return           Always 1.
 *  Stack: [..., metatable_or_nil] → [...] */
int lua_setmetatable(lua_State* L, int objindex)
{
    api_checknelems(L, 1);
    TValue* obj = index2addr(L, objindex);
    api_checkvalidindex(L, obj);
    LuaTable* mt = NULL;
    if (!ttisnil(L->top - 1))
    {
        api_check(L, ttistable(L->top - 1));
        mt = hvalue(L->top - 1);
    }
    switch (ttype(obj))
    {
    case LUA_TTABLE:
    {
        if (hvalue(obj)->readonly)
            luaG_readonlyerror(L);
        hvalue(obj)->metatable = mt;
        if (mt)
            luaC_objbarrier(L, hvalue(obj), mt);
        break;
    }
    case LUA_TUSERDATA:
    {
        uvalue(obj)->metatable = mt;
        if (mt)
            luaC_objbarrier(L, uvalue(obj), mt);
        break;
    }
    default:
    {
        L->global->mt[ttype(obj)] = mt;
        break;
    }
    }
    L->top--;
    return 1;
}

/** @brief Pops a table and sets it as the environment of the function/thread at `idx`.
 *
 *  @warning **Luau restriction**: only works on `LUA_TFUNCTION` and `LUA_TTHREAD`.
 *  For userdata, returns 0 and does nothing — this is why the AwesomeWM shim
 *  replaces `lua_setfenv` on userdata with the registry-keyed-by-pointer pattern.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the function or thread.
 *  @return     1 on success, 0 if the value type does not support environments.
 *  Stack: [..., table] → [...] */
int lua_setfenv(lua_State* L, int idx)
{
    int res = 1;
    api_checknelems(L, 1);
    StkId o = index2addr(L, idx);
    api_checkvalidindex(L, o);
    api_check(L, ttistable(L->top - 1));
    switch (ttype(o))
    {
    case LUA_TFUNCTION:
        clvalue(o)->env = hvalue(L->top - 1);
        break;
    case LUA_TTHREAD:
        thvalue(o)->gt = hvalue(L->top - 1);
        break;
    default:
        res = 0;
        break;
    }
    if (res)
    {
        luaC_objbarrier(L, &gcvalue(o)->gch, hvalue(L->top - 1));
    }
    L->top--;
    return res;
}

/*
** `load' and `call' functions (run Lua code)
*/

#define adjustresults(L, nres) \
    { \
        if (nres == LUA_MULTRET && L->top >= L->ci->top) \
            L->ci->top = L->top; \
    }

#define checkresults(L, na, nr) api_check(L, (nr) == LUA_MULTRET || (L->ci->top - L->top >= (nr) - (na)))

/** @brief Calls a Lua function — UNPROTECTED; errors propagate as C longjmps.
 *
 *  The function and its `nargs` arguments must already be on the stack (function
 *  at the bottom, arguments above it).  After the call they are all replaced by
 *  exactly `nresults` return values (or all results if `nresults == LUA_MULTRET`).
 *  If an error occurs, it propagates past this call frame — use `lua_pcall` if
 *  you need to catch errors.
 *
 *  @param L        The Lua state.
 *  @param nargs    Number of arguments on the stack.
 *  @param nresults Expected number of return values (or LUA_MULTRET for all).
 *  Stack: [..., func, arg1, ..., argN] → [..., ret1, ..., retR] */
void lua_call(lua_State* L, int nargs, int nresults)
{
    StkId func;
    api_checknelems(L, nargs + 1);
    api_check(L, L->status == 0);
    checkresults(L, nargs, nresults);
    func = L->top - (nargs + 1);

    luaD_call(L, func, nresults);

    adjustresults(L, nresults);
}

/*
** Execute a protected call.
*/
// data to `f_call'
struct CallS
{
    StkId func;
    int nresults;
};

static void f_call(lua_State* L, void* ud)
{
    struct CallS* c = cast_to(struct CallS*, ud);
    luaD_call(L, c->func, c->nresults);
}

/** @brief Calls a Lua function in protected mode; catches errors.
 *
 *  Like `lua_call` but wraps the call in a C `setjmp` guard.  On error,
 *  the stack is restored to its state before the call, the error object is
 *  pushed, and a non-zero status code is returned.
 *
 *  - `errfunc`: if non-zero, the stack index of a message handler function
 *    called before the stack unwinds (used to add traceback info).  Pass 0
 *    for no handler.
 *
 *  @param L        The Lua state.
 *  @param nargs    Number of arguments.
 *  @param nresults Expected return values (or LUA_MULTRET).
 *  @param errfunc  Stack index of error handler, or 0.
 *  @return         LUA_OK (0) on success, or LUA_ERRRUN/LUA_ERRMEM/LUA_ERRERR.
 *  Stack (success): [..., func, args] → [..., rets]
 *  Stack (error):   [..., func, args] → [..., err_object] */
int lua_pcall(lua_State* L, int nargs, int nresults, int errfunc)
{
    api_checknelems(L, nargs + 1);
    api_check(L, L->status == 0);
    checkresults(L, nargs, nresults);
    ptrdiff_t func = 0;
    if (errfunc != 0)
    {
        StkId o = index2addr(L, errfunc);
        api_checkvalidindex(L, o);
        func = savestack(L, o);
    }
    struct CallS c;
    c.func = L->top - (nargs + 1); // function to be called
    c.nresults = nresults;

    int status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);

    adjustresults(L, nresults);
    return status;
}

/*
** Execute a protected C call.
*/
// data to `f_Ccall'
struct CCallS
{
    lua_CFunction func;
    void* ud;
};

static void f_Ccall(lua_State* L, void* ud)
{
    struct CCallS* c = cast_to(struct CCallS*, ud);

    if (!lua_checkstack(L, 2))
        luaG_runerror(L, "stack limit");

    lua_pushcclosurek(L, c->func, nullptr, 0, nullptr);
    lua_pushlightuserdata(L, c->ud);
    luaD_call(L, L->top - 2, 0);
}

/** @brief Calls a C function in protected mode, passing a `void*` userdata pointer.
 *
 *  Pushes `func` as a C closure and `ud` as a light userdata, then calls
 *  with 0 results in a protected context.  Does NOT push function + args onto
 *  the stack beforehand — the C function receives `ud` via its first argument.
 *  Useful for one-shot protected setup calls.
 *
 *  @param L     The Lua state.
 *  @param func  The C function to call.
 *  @param ud    Arbitrary pointer passed to `func` as a light userdata argument.
 *  @return      LUA_OK on success, or an error status code. */
int lua_cpcall(lua_State* L, lua_CFunction func, void* ud)
{
    api_check(L, L->status == 0);

    struct CCallS c;
    c.func = func;
    c.ud = ud;

    return luaD_pcall(L, f_Ccall, &c, savestack(L, L->top), 0);
}

/** @brief Returns the current status code of the Lua state.
 *
 *  - `LUA_OK` (0): normal/idle.
 *  - `LUA_YIELD`: the coroutine has yielded and is waiting to be resumed.
 *  - `LUA_BREAK`: internal coroutine break state (used by the debugger).
 *  - Non-zero error code: the coroutine died with an error.
 *
 *  @param L  The Lua state.
 *  @return   LUA_OK, LUA_YIELD, LUA_BREAK, or an error status. */
int lua_status(lua_State* L)
{
    return L->status;
}

/** @brief Returns the coroutine status of `co` as seen from `L`.
 *
 *  - `LUA_CORUN`:  `co` is currently running (co == L).
 *  - `LUA_COSUS`:  suspended (yielded or not yet started).
 *  - `LUA_CONOR`:  normal — suspended but has active call frames (resumed another coroutine).
 *  - `LUA_COERR`:  dead due to an unhandled error.
 *  - `LUA_COFIN`:  finished (returned normally, stack is empty).
 *
 *  @param L   The running Lua state (for context).
 *  @param co  The coroutine to inspect.
 *  @return    One of the LUA_CO* status constants. */
int lua_costatus(lua_State* L, lua_State* co)
{
    if (co == L)
        return LUA_CORUN;
    if (co->status == LUA_YIELD)
        return LUA_COSUS;
    if (co->status == LUA_BREAK)
        return LUA_CONOR;
    if (co->status != 0) // some error occurred
        return LUA_COERR;
    if (co->ci != co->base_ci) // does it have frames?
        return LUA_CONOR;
    if (co->top == co->base)
        return LUA_COFIN;
    return LUA_COSUS; // initial state
}

/** @brief Returns the C-side userdata pointer attached to this thread.
 *
 *  Each lua_State has a `void* userdata` slot for the embedding application
 *  to store per-thread state (e.g. a context struct).  Returns NULL if not set.
 *
 *  @param L  The Lua state.
 *  @return   The userdata pointer, or NULL. */
void* lua_getthreaddata(lua_State* L)
{
    return L->userdata;
}

/** @brief Attaches an arbitrary C pointer to this thread for embedder use.
 *
 *  The VM does not use or interpret this pointer — it is entirely for the
 *  host application.  Retrieve it later with `lua_getthreaddata`.
 *
 *  @param L     The Lua state.
 *  @param data  Arbitrary pointer to store. */
void lua_setthreaddata(lua_State* L, void* data)
{
    L->userdata = data;
}

/*
** Garbage-collection function
*/

/** @brief GC control interface — query stats, trigger collection, tune parameters.
 *
 *  `what` selects the operation:
 *  - `LUA_GCSTOP`       — disable automatic GC.
 *  - `LUA_GCRESTART`    — re-enable automatic GC.
 *  - `LUA_GCCOLLECT`    — run a full GC cycle immediately.
 *  - `LUA_GCCOUNT`      — return total GC memory in KB.
 *  - `LUA_GCCOUNTB`     — return the fractional byte portion of GC memory.
 *  - `LUA_GCISRUNNING`  — return 1 if GC is running, 0 if stopped.
 *  - `LUA_GCSTEP`       — perform `data` KB of incremental GC work.
 *  - `LUA_GCSETGOAL`    — set the GC heap growth goal percentage; returns old value.
 *  - `LUA_GCSETSTEPMUL` — set GC step multiplier; returns old value.
 *  - `LUA_GCSETSTEPSIZE`— set GC step size in KB; returns old value.
 *
 *  @param L     The Lua state.
 *  @param what  Operation selector (LUA_GC* constant).
 *  @param data  Operation-specific integer parameter.
 *  @return      Operation-specific integer result, or -1 for invalid `what`. */
int lua_gc(lua_State* L, int what, int data)
{
    int res = 0;
    condhardmemtests(luaC_validate(L), 1);
    global_State* g = L->global;
    switch (what)
    {
    case LUA_GCSTOP:
    {
        g->GCthreshold = SIZE_MAX;
        break;
    }
    case LUA_GCRESTART:
    {
        g->GCthreshold = g->totalbytes;
        break;
    }
    case LUA_GCCOLLECT:
    {
        luaC_fullgc(L);
        break;
    }
    case LUA_GCCOUNT:
    {
        // GC values are expressed in Kbytes: #bytes/2^10
        res = cast_int(g->totalbytes >> 10);
        break;
    }
    case LUA_GCCOUNTB:
    {
        res = cast_int(g->totalbytes & 1023);
        break;
    }
    case LUA_GCISRUNNING:
    {
        res = (g->GCthreshold != SIZE_MAX);
        break;
    }
    case LUA_GCSTEP:
    {
        size_t amount = (cast_to(size_t, data) << 10);
        ptrdiff_t oldcredit = g->gcstate == GCSpause ? 0 : g->GCthreshold - g->totalbytes;

        // temporarily adjust the threshold so that we can perform GC work
        if (amount <= g->totalbytes)
            g->GCthreshold = g->totalbytes - amount;
        else
            g->GCthreshold = 0;

#ifdef LUAI_GCMETRICS
        double startmarktime = g->gcmetrics.currcycle.marktime;
        double startsweeptime = g->gcmetrics.currcycle.sweeptime;
#endif

        // track how much work the loop will actually perform
        size_t actualwork = 0;

        while (g->GCthreshold <= g->totalbytes)
        {
            size_t stepsize = luaC_step(L, false);

            actualwork += stepsize;

            if (g->gcstate == GCSpause)
            {            // end of cycle?
                res = 1; // signal it
                break;
            }
        }

#ifdef LUAI_GCMETRICS
        // record explicit step statistics
        GCCycleMetrics* cyclemetrics = g->gcstate == GCSpause ? &g->gcmetrics.lastcycle : &g->gcmetrics.currcycle;

        double totalmarktime = cyclemetrics->marktime - startmarktime;
        double totalsweeptime = cyclemetrics->sweeptime - startsweeptime;

        if (totalmarktime > 0.0)
        {
            cyclemetrics->markexplicitsteps++;

            if (totalmarktime > cyclemetrics->markmaxexplicittime)
                cyclemetrics->markmaxexplicittime = totalmarktime;
        }

        if (totalsweeptime > 0.0)
        {
            cyclemetrics->sweepexplicitsteps++;

            if (totalsweeptime > cyclemetrics->sweepmaxexplicittime)
                cyclemetrics->sweepmaxexplicittime = totalsweeptime;
        }
#endif

        // if cycle hasn't finished, advance threshold forward for the amount of extra work performed
        if (g->gcstate != GCSpause)
        {
            // if a new cycle was triggered by explicit step, old 'credit' of GC work is 0
            ptrdiff_t newthreshold = g->totalbytes + actualwork + oldcredit;
            g->GCthreshold = newthreshold < 0 ? 0 : newthreshold;
        }
        break;
    }
    case LUA_GCSETGOAL:
    {
        res = g->gcgoal;
        g->gcgoal = data;
        break;
    }
    case LUA_GCSETSTEPMUL:
    {
        res = g->gcstepmul;
        g->gcstepmul = data;
        break;
    }
    case LUA_GCSETSTEPSIZE:
    {
        // GC values are expressed in Kbytes: #bytes/2^10
        res = g->gcstepsize >> 10;
        g->gcstepsize = data << 10;
        break;
    }
    default:
        res = -1; // invalid option
    }
    return res;
}

/*
** miscellaneous functions
*/

/** @brief Throws the value at the top of the stack as a runtime error.
 *
 *  Does NOT pop the value — it becomes the error object propagated to the
 *  nearest `lua_pcall` handler (or terminates the program if unprotected).
 *  Never returns.  The value can be any type, but is conventionally a string.
 *
 *  @param L  The Lua state (top value is the error object).
 *  Stack: [..., err_object]  (consumed by error propagation) */
l_noret lua_error(lua_State* L)
{
    api_checknelems(L, 1);

    luaD_throw(L, LUA_ERRRUN);
}

/** @brief Advances a table iteration; pops the previous key, pushes next key+value.
 *
 *  Push a nil key before the first call to start iteration.  Each call pops
 *  the key at the top of the stack and either:
 *  - Pushes the next key and value (returns 1), OR
 *  - Pops the key and returns 0 (iteration finished).
 *
 *  @note Table must not be modified during iteration.
 *  @note For performance-critical iteration, prefer `lua_rawiter`.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table.
 *  @return     1 if more entries remain, 0 if done.
 *  Stack (more):  [..., key] → [..., next_key, value]
 *  Stack (done):  [..., key] → [...] */
int lua_next(lua_State* L, int idx)
{
    luaC_threadbarrier(L);
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    int more = luaH_next(L, hvalue(t), L->top - 1);
    if (more)
    {
        api_incr_top(L);
    }
    else             // no more elements
        L->top -= 1; // remove key
    return more;
}

/** @brief Luau-specific stateless table iterator using an integer cursor.
 *
 *  More efficient than `lua_next` because it avoids key hashing and doesn't
 *  require the key to be on the stack.  Pass `iter = 0` to start; pass the
 *  returned value as `iter` for each subsequent call.
 *
 *  Returns -1 when iteration is complete.  On each successful step, pushes
 *  the key and value onto the stack.
 *
 *  @param L     The Lua state.
 *  @param idx   Stack index of the table.
 *  @param iter  Iteration cursor (0 to start, or the previous return value).
 *  @return      New cursor value, or -1 if iteration is complete.
 *  Stack (more):  [...] → [..., key, value]
 *  Stack (done):  [...] → [...] */
int lua_rawiter(lua_State* L, int idx, int iter)
{
    luaC_threadbarrier(L);
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    api_check(L, iter >= 0);

    LuaTable* h = hvalue(t);
    int sizearray = h->sizearray;

    // first we advance iter through the array portion
    for (; unsigned(iter) < unsigned(sizearray); ++iter)
    {
        TValue* e = &h->array[iter];

        if (!ttisnil(e))
        {
            StkId top = L->top;
            setnvalue(top + 0, double(iter + 1));
            setobj2s(L, top + 1, e);
            api_update_top(L, top + 2);
            return iter + 1;
        }
    }

    int sizenode = 1 << h->lsizenode;

    // then we advance iter through the hash portion
    for (; unsigned(iter - sizearray) < unsigned(sizenode); ++iter)
    {
        LuaNode* n = &h->node[iter - sizearray];

        if (!ttisnil(gval(n)))
        {
            StkId top = L->top;
            getnodekey(L, top + 0, n);
            setobj2s(L, top + 1, gval(n));
            api_update_top(L, top + 2);
            return iter + 1;
        }
    }

    // traversal finished
    return -1;
}

/** @brief Concatenates the top `n` string/number values on the stack.
 *
 *  Pops `n` values and pushes a single concatenated string.  Values are
 *  concatenated left-to-right (bottom → top).  Numbers are coerced to strings.
 *  Special cases: `n == 0` pushes an empty string; `n == 1` is a no-op.
 *  Does NOT invoke `__concat` metamethods.
 *
 *  @param L  The Lua state.
 *  @param n  Number of values to concatenate (must all be strings or numbers).
 *  Stack: [..., s1, s2, ..., sN] → [..., s1..s2..sN] */
void lua_concat(lua_State* L, int n)
{
    api_checknelems(L, n);
    if (n >= 2)
    {
        luaC_checkGC(L);
        luaC_threadbarrier(L);
        luaV_concat(L, n, cast_int(L->top - L->base) - 1);
        L->top -= (n - 1);
    }
    else if (n == 0)
    { // push empty string
        luaC_threadbarrier(L);
        setsvalue(L, L->top, luaS_newlstr(L, "", 0));
        api_incr_top(L);
    }
    // else n == 1; nothing to do
}

/** @brief Allocates a GC-managed userdata block of `sz` bytes with a numeric tag.
 *
 *  Pushes a new full userdata onto the stack and returns a pointer to its
 *  embedded data block.  Write your C struct into the returned pointer.
 *
 *  The `tag` (0 to LUA_UTAG_LIMIT-1) enables fast type dispatch via
 *  `lua_touserdatatagged` and per-tag destructors via `lua_setuserdatadtor`.
 *  `UTAG_PROXY` is a special reserved tag.
 *
 *  @param L    The Lua state.
 *  @param sz   Size in bytes of the userdata payload.
 *  @param tag  Numeric type tag.
 *  @return     Pointer to the `sz`-byte data block (GC-managed lifetime).
 *  Stack: [...] → [..., userdata] */
void* lua_newuserdatatagged(lua_State* L, size_t sz, int tag)
{
    api_check(L, unsigned(tag) < LUA_UTAG_LIMIT || tag == UTAG_PROXY);
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    Udata* u = luaU_newudata(L, sz, tag);
    setuvalue(L, L->top, u);
    api_incr_top(L);
    return u->data;
}

/** @brief Like `lua_newuserdatatagged` but automatically assigns the pre-registered metatable.
 *
 *  The metatable for `tag` must have been registered first via
 *  `lua_setuserdatametatable`.  Asserts in debug builds if not registered.
 *  The metatable assignment is done without a GC barrier (the object is newly
 *  allocated white, so no barrier is needed).
 *
 *  @param L    The Lua state.
 *  @param sz   Size in bytes of the userdata payload.
 *  @param tag  Numeric type tag (must have a registered metatable).
 *  @return     Pointer to the `sz`-byte data block.
 *  Stack: [...] → [..., userdata] */
void* lua_newuserdatataggedwithmetatable(lua_State* L, size_t sz, int tag)
{
    api_check(L, unsigned(tag) < LUA_UTAG_LIMIT);
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    Udata* u = luaU_newudata(L, sz, tag);

    // currently, we always allocate unmarked objects, so forward barrier can be skipped
    LUAU_ASSERT(!isblack(obj2gco(u)));

    LuaTable* h = L->global->udatamt[tag];
    api_check(L, h != nullptr);

    u->metatable = h;

    setuvalue(L, L->top, u);
    api_incr_top(L);
    return u->data;
}

/** @brief Allocates a userdata with an inline C destructor callback.
 *
 *  The destructor `dtor` is called by the GC when the userdata is collected,
 *  receiving the data pointer as its argument.  The destructor pointer is
 *  stored in the allocation immediately after `sz` bytes of user data
 *  (using `UTAG_IDTOR` as the internal tag).
 *
 *  Use this when you need RAII-style cleanup (e.g. closing a file handle)
 *  without registering a per-tag destructor via `lua_setuserdatadtor`.
 *
 *  @param L     The Lua state.
 *  @param sz    Size in bytes of the userdata payload.
 *  @param dtor  Destructor function called at GC time with the data pointer.
 *  @return      Pointer to the `sz`-byte data block.
 *  Stack: [...] → [..., userdata] */
void* lua_newuserdatadtor(lua_State* L, size_t sz, void (*dtor)(void*))
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    // make sure sz + sizeof(dtor) doesn't overflow; luaU_newdata will reject SIZE_MAX correctly
    size_t as = sz < SIZE_MAX - sizeof(dtor) ? sz + sizeof(dtor) : SIZE_MAX;
    Udata* u = luaU_newudata(L, as, UTAG_IDTOR);
    memcpy(&u->data + sz, &dtor, sizeof(dtor));
    setuvalue(L, L->top, u);
    api_incr_top(L);
    return u->data;
}

/** @brief Allocates a new Luau buffer object of `sz` bytes and pushes it.
 *
 *  Luau buffers (`LUA_TBUFFER`) are mutable, fixed-size byte arrays — think
 *  of them as a GC-managed `uint8_t[sz]`.  Access the data via `lua_tobuffer`.
 *  Unlike userdata they have no tag, metatable, or destructor.
 *
 *  @param L   The Lua state.
 *  @param sz  Byte size of the buffer.
 *  @return    Pointer to the `sz`-byte data region.
 *  Stack: [...] → [..., buffer] */
void* lua_newbuffer(lua_State* L, size_t sz)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    Buffer* b = luaB_newbuffer(L, sz);
    setbufvalue(L, L->top, b);
    api_incr_top(L);
    return b->data;
}

static const char* aux_upvalue(StkId fi, int n, TValue** val)
{
    Closure* f;
    if (!ttisfunction(fi))
        return NULL;
    f = clvalue(fi);
    if (f->isC)
    {
        if (!(1 <= n && n <= f->nupvalues))
            return NULL;
        *val = &f->c.upvals[n - 1];
        return "";
    }
    else
    {
        Proto* p = f->l.p;
        if (!(1 <= n && n <= p->nups)) // not a valid upvalue
            return NULL;
        TValue* r = &f->l.uprefs[n - 1];
        *val = ttisupval(r) ? upvalue(r)->v : r;
        if (!(1 <= n && n <= p->sizeupvalues)) // don't have a name for this upvalue
            return "";
        return getstr(p->upvalues[n - 1]);
    }
}

/** @brief Pushes the n-th upvalue of the function at `funcindex`.
 *
 *  Upvalues are 1-indexed.  For C closures, returns "" as the name (upvalue
 *  names are only stored for Lua closures).  Returns NULL and pushes nothing
 *  if `n` is out of range.
 *
 *  @param L          The Lua state.
 *  @param funcindex  Stack index of the function.
 *  @param n          1-based upvalue index.
 *  @return           Upvalue name (or "" for C closures), or NULL if out of range.
 *  Stack (success): [...] → [..., upvalue] */
const char* lua_getupvalue(lua_State* L, int funcindex, int n)
{
    luaC_threadbarrier(L);
    TValue* val;
    const char* name = aux_upvalue(index2addr(L, funcindex), n, &val);
    if (name)
    {
        setobj2s(L, L->top, val);
        api_incr_top(L);
    }
    return name;
}

/** @brief Pops the top value and sets it as the n-th upvalue of the function.
 *
 *  Upvalues are 1-indexed.  Returns NULL and does nothing if `n` is out of
 *  range.  Returns "" for C closures (which have unnamed upvalues).
 *
 *  @param L          The Lua state.
 *  @param funcindex  Stack index of the function.
 *  @param n          1-based upvalue index.
 *  @return           Upvalue name (or "" for C closures), or NULL if out of range.
 *  Stack: [..., new_value] → [...] */
const char* lua_setupvalue(lua_State* L, int funcindex, int n)
{
    api_checknelems(L, 1);
    StkId fi = index2addr(L, funcindex);
    TValue* val;
    const char* name = aux_upvalue(fi, n, &val);
    if (name)
    {
        L->top--;
        setobj(L, val, L->top);
        luaC_barrier(L, clvalue(fi), L->top);
    }
    return name;
}

/** @brief Obfuscates a raw pointer for safe display in Lua (e.g. tostring output).
 *
 *  Applies a VM-wide random linear transformation to `p` so the actual ASLR
 *  address is not exposed to scripts.  The encoding is consistent within one
 *  VM lifetime but varies across runs.  Used by `luaL_tolstring` for the
 *  default "TYPE: 0xADDR" formatting.
 *
 *  @param L  The Lua state.
 *  @param p  Raw pointer value to encode.
 *  @return   Encoded (obfuscated) pointer value. */
uintptr_t lua_encodepointer(lua_State* L, uintptr_t p)
{
    global_State* g = L->global;
    return uintptr_t((g->ptrenckey[0] * p + g->ptrenckey[2]) ^ (g->ptrenckey[1] * p + g->ptrenckey[3]));
}

/** @brief Creates a GC-rooted integer reference to the value at stack index `idx`.
 *
 *  Stores the value in the registry under a new integer key and returns that
 *  key as the "ref" handle.  The value is protected from GC until `lua_unref`
 *  is called.  Returns `LUA_REFNIL` if the value is nil.
 *
 *  This is the standard Luau way to hold a Lua value in C across multiple API
 *  calls (replacing `luaL_ref`/`luaL_unref` from standard Lua).
 *
 *  To retrieve the value: `lua_rawgeti(L, LUA_REGISTRYINDEX, ref)`.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the value to reference (NOT removed from stack).
 *  @return     An integer ref handle, or LUA_REFNIL. */
int lua_ref(lua_State* L, int idx)
{
    api_check(L, idx != LUA_REGISTRYINDEX); // idx is a stack index for value
    int ref = LUA_REFNIL;
    global_State* g = L->global;
    StkId p = index2addr(L, idx);
    if (!ttisnil(p))
    {
        LuaTable* reg = hvalue(registry(L));

        if (g->registryfree != 0)
        { // reuse existing slot
            ref = g->registryfree;
        }
        else
        { // no free elements
            ref = luaH_getn(reg);
            ref++; // create new reference
        }

        TValue* slot = luaH_setnum(L, reg, ref);
        if (g->registryfree != 0)
            g->registryfree = int(nvalue(slot));
        setobj2t(L, slot, p);
        luaC_barriert(L, reg, p);
    }
    return ref;
}

/** @brief Releases a reference created by `lua_ref`, allowing the value to be GC'd.
 *
 *  Frees the registry slot associated with `ref` for reuse.  Safe to call with
 *  `LUA_REFNIL` or any value ≤ `LUA_REFNIL` (no-op).  After calling this, the
 *  ref handle is invalid — do not use it again.
 *
 *  @param L    The Lua state.
 *  @param ref  Reference handle returned by a previous `lua_ref` call. */
void lua_unref(lua_State* L, int ref)
{
    if (ref <= LUA_REFNIL)
        return;

    global_State* g = L->global;
    LuaTable* reg = hvalue(registry(L));

    const TValue* slot = luaH_getnum(reg, ref);
    api_check(L, slot != luaO_nilobject);

    // similar to how 'luaH_setnum' makes non-nil slot value mutable
    TValue* mutableSlot = (TValue*)slot;

    // NB: no barrier needed because value isn't collectable
    setnvalue(mutableSlot, g->registryfree);

    g->registryfree = ref;
}

/** @brief Changes the numeric tag of the full userdata at `idx`.
 *
 *  Tag must be in range 0 to LUA_UTAG_LIMIT-1.  Changing the tag affects
 *  subsequent `lua_touserdatatagged` checks and which per-tag destructor fires.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the full userdata.
 *  @param tag  New tag value. */
void lua_setuserdatatag(lua_State* L, int idx, int tag)
{
    api_check(L, unsigned(tag) < LUA_UTAG_LIMIT);
    StkId o = index2addr(L, idx);
    api_check(L, ttisuserdata(o));
    uvalue(o)->tag = uint8_t(tag);
}

/** @brief Registers a C destructor for ALL full userdata with the given tag.
 *
 *  The destructor is called by the GC when any userdata with this tag is
 *  collected.  One destructor per tag; calling again overwrites the previous.
 *  This is the per-type cleanup mechanism (as opposed to the per-object
 *  `lua_newuserdatadtor` approach).
 *
 *  @param L     The Lua state.
 *  @param tag   Userdata tag (0 to LUA_UTAG_LIMIT-1).
 *  @param dtor  Destructor: `void dtor(lua_State* L, void* data)`. */
void lua_setuserdatadtor(lua_State* L, int tag, lua_Destructor dtor)
{
    api_check(L, unsigned(tag) < LUA_UTAG_LIMIT);
    L->global->udatagc[tag] = dtor;
}

/** @brief Retrieves the destructor registered for userdata with the given tag.
 *
 *  @param L    The Lua state.
 *  @param tag  Userdata tag (0 to LUA_UTAG_LIMIT-1).
 *  @return     The registered lua_Destructor, or NULL if none. */
lua_Destructor lua_getuserdatadtor(lua_State* L, int tag)
{
    api_check(L, unsigned(tag) < LUA_UTAG_LIMIT);
    return L->global->udatagc[tag];
}

/** @brief Registers the top-of-stack table as the default metatable for userdata `tag`.
 *
 *  Pops the table and stores it in `L->global->udatamt[tag]`.  Can only be
 *  called once per tag (reassignment asserts in debug builds).  After this,
 *  `lua_newuserdatataggedwithmetatable` will auto-assign this metatable to
 *  all new userdata of that tag.
 *
 *  @param L    The Lua state.
 *  @param tag  Userdata tag (0 to LUA_UTAG_LIMIT-1).
 *  Stack: [..., metatable] → [...] */
void lua_setuserdatametatable(lua_State* L, int tag)
{
    api_check(L, unsigned(tag) < LUA_UTAG_LIMIT);
    api_check(L, !L->global->udatamt[tag]); // reassignment not supported
    api_check(L, ttistable(L->top - 1));
    L->global->udatamt[tag] = hvalue(L->top - 1);
    L->top--;
}

/** @brief Pushes the default metatable registered for userdata `tag`, or nil.
 *
 *  @param L    The Lua state.
 *  @param tag  Userdata tag (0 to LUA_UTAG_LIMIT-1).
 *  Stack: [...] → [..., metatable_or_nil] */
void lua_getuserdatametatable(lua_State* L, int tag)
{
    api_check(L, unsigned(tag) < LUA_UTAG_LIMIT);
    luaC_threadbarrier(L);

    if (LuaTable* h = L->global->udatamt[tag])
    {
        sethvalue(L, L->top, h);
    }
    else
    {
        setnilvalue(L->top);
    }

    api_incr_top(L);
}

/** @brief Associates a type name string with a light userdata tag.
 *
 *  Once set, `lua_typename` and error messages will use this name for light
 *  userdata of this tag.  Can only be called once per tag.  The name string
 *  is pinned (never collected by the GC).
 *
 *  @param L     The Lua state.
 *  @param tag   Light userdata tag (0 to LUA_LUTAG_LIMIT-1).
 *  @param name  Human-readable type name (e.g. "MyHandle"). */
void lua_setlightuserdataname(lua_State* L, int tag, const char* name)
{
    api_check(L, unsigned(tag) < LUA_LUTAG_LIMIT);
    api_check(L, !L->global->lightuserdataname[tag]); // renaming not supported
    if (!L->global->lightuserdataname[tag])
    {
        L->global->lightuserdataname[tag] = luaS_new(L, name);
        luaS_fix(L->global->lightuserdataname[tag]); // never collect these names
    }
}

/** @brief Returns the type name string registered for a light userdata tag, or NULL.
 *
 *  @param L    The Lua state.
 *  @param tag  Light userdata tag (0 to LUA_LUTAG_LIMIT-1).
 *  @return     The registered name string, or NULL if unset. */
const char* lua_getlightuserdataname(lua_State* L, int tag)
{
    api_check(L, unsigned(tag) < LUA_LUTAG_LIMIT);
    const TString* name = L->global->lightuserdataname[tag];
    return name ? getstr(name) : nullptr;
}

/** @brief Pushes a shallow clone of the Lua closure at `idx`.
 *
 *  Creates a new Closure object that shares the same `Proto` (bytecode +
 *  constants) as the original but gets a fresh upvalue array initialised
 *  from the original's current upvalue values.  The clone is independent —
 *  subsequent upvalue mutations in one do not affect the other.
 *
 *  Useful for sandboxing: give each untrusted script a clone of a function
 *  so its upvalue state cannot leak between invocations.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the Lua closure to clone (must be isLfunction).
 *  Stack: [...] → [..., cloned_closure] */
void lua_clonefunction(lua_State* L, int idx)
{
    luaC_checkGC(L);
    luaC_threadbarrier(L);
    StkId p = index2addr(L, idx);
    api_check(L, isLfunction(p));
    Closure* cl = clvalue(p);
    Closure* newcl = luaF_newLclosure(L, cl->nupvalues, L->gt, cl->l.p);
    for (int i = 0; i < cl->nupvalues; ++i)
        setobj2n(L, &newcl->l.uprefs[i], &cl->l.uprefs[i]);
    setclvalue(L, L->top, newcl);
    api_incr_top(L);
}

/** @brief Removes all key-value entries from the table at `idx`.
 *
 *  Equivalent to iterating and nilling every key, but more efficient.
 *  Throws if the table is read-only.  Does not release the table's allocated
 *  hash/array capacity — the memory stays reserved.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table to clear. */
void lua_cleartable(lua_State* L, int idx)
{
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));
    LuaTable* tt = hvalue(t);
    if (tt->readonly)
        luaG_readonlyerror(L);
    luaH_clear(tt);
}

/** @brief Pushes a shallow copy of the table at `idx`.
 *
 *  Creates a new table with the same keys and values (one level deep — nested
 *  tables are NOT cloned, only the references are copied).  The new table
 *  inherits the same hash/array layout but is a completely separate object.
 *
 *  @param L    The Lua state.
 *  @param idx  Stack index of the table to clone.
 *  Stack: [...] → [..., cloned_table] */
void lua_clonetable(lua_State* L, int idx)
{
    StkId t = index2addr(L, idx);
    api_check(L, ttistable(t));

    LuaTable* tt = luaH_clone(L, hvalue(t));
    sethvalue(L, L->top, tt);
    api_incr_top(L);
}

/** @brief Returns the `lua_Callbacks*` struct for hooking into VM events.
 *
 *  The `lua_Callbacks` struct (defined in `lualib.h`) exposes function
 *  pointers called by the VM at key points:
 *  - `interrupt`: called periodically during bytecode execution (debugger hook).
 *  - `panic`: called on an unprotected error.
 *  - `userthread`: called when a new coroutine is created.
 *  - `useratom`: called to assign integer atoms to interned strings.
 *  - `debugbreak`, `debugstep`, etc.: debugger integration hooks.
 *
 *  @param L  The Lua state.
 *  @return   Pointer to the global lua_Callbacks struct (modify fields directly). */
lua_Callbacks* lua_callbacks(lua_State* L)
{
    return &L->global->cb;
}

/** @brief Sets the memory accounting category for subsequent allocations.
 *
 *  Luau tracks memory usage per category (0 to LUA_MEMORY_CATEGORIES-1).
 *  Setting this before allocating userdata, buffers, etc. lets you query
 *  per-category usage via `lua_totalbytes(L, category)`.
 *
 *  @param L         The Lua state.
 *  @param category  Memory category index (0 to LUA_MEMORY_CATEGORIES-1). */
void lua_setmemcat(lua_State* L, int category)
{
    api_check(L, unsigned(category) < LUA_MEMORY_CATEGORIES);
    L->activememcat = uint8_t(category);
}

/** @brief Returns total GC-tracked memory usage in bytes.
 *
 *  - `category < 0`: returns total bytes across all categories.
 *  - `category >= 0`: returns bytes charged to that specific category.
 *
 *  @param L         The Lua state.
 *  @param category  Memory category (-1 for total, 0+ for per-category).
 *  @return          Byte count. */
size_t lua_totalbytes(lua_State* L, int category)
{
    api_check(L, category < LUA_MEMORY_CATEGORIES);
    return category < 0 ? L->global->totalbytes : L->global->memcatbytes[category];
}

/** @brief Returns the allocator function and its userdata pointer.
 *
 *  Retrieves the `lua_Alloc` function set at VM creation time (via
 *  `lua_newstate`).  If `ud` is non-NULL, the allocator's userdata pointer
 *  is written there.
 *
 *  @param L   The Lua state.
 *  @param ud  Out-param for the allocator's userdata (may be NULL).
 *  @return    The lua_Alloc function pointer. */
lua_Alloc lua_getallocf(lua_State* L, void** ud)
{
    lua_Alloc f = L->global->frealloc;
    if (ud)
        *ud = L->global->ud;
    return f;
}
