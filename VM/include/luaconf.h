// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

// When debugging complex issues, consider enabling one of these:
// This will reallocate the stack very aggressively at every opportunity; use this with asan to catch stale stack pointers
// #define HARDSTACKTESTS 1
// This will call GC validation very aggressively at every incremental GC step; use this with caution as it's SLOW
// #define HARDMEMTESTS 1
// This will call GC validation very aggressively at every GC opportunity; use this with caution as it's VERY SLOW
// #define HARDMEMTESTS 2

// To force MSVC2017+ to generate SSE2 code for some stdlib functions we need to locally enable /fp:fast
// Note that /fp:fast changes the semantics of floating point comparisons so this is only safe to do for functions without ones
#if defined(_MSC_VER) && !defined(__clang__)
/** @brief Opens an MSVC /fp:fast region to allow SSE2 optimisation of math intrinsics.
 *  Pair with LUAU_FASTMATH_END.  Only active under MSVC; expands to nothing elsewhere.
 *  Do NOT use around code that contains floating-point comparisons — /fp:fast alters
 *  their semantics. */
#define LUAU_FASTMATH_BEGIN __pragma(float_control(precise, off, push))
/** @brief Closes the MSVC /fp:fast region opened by LUAU_FASTMATH_BEGIN.
 *  Restores the previous /fp setting via the compiler's float_control stack. */
#define LUAU_FASTMATH_END __pragma(float_control(pop))
#else
#define LUAU_FASTMATH_BEGIN
#define LUAU_FASTMATH_END
#endif

// Some functions like floor/ceil have SSE4.1 equivalents but we currently support systems without SSE4.1
// Note that we only need to do this when SSE4.1 support is not guaranteed by compiler settings, as otherwise compiler will optimize these for us.
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__SSE4_1__) && !defined(__AVX__)
#if defined(_MSC_VER) && !defined(__clang__)
#define LUAU_TARGET_SSE41
#elif defined(__GNUC__) && defined(__has_attribute)
#if __has_attribute(target)
#define LUAU_TARGET_SSE41 __attribute__((target("sse4.1")))
#endif
#endif
#endif

// Used on functions that have a printf-like interface to validate them statically
#if defined(__GNUC__)
/** @brief Annotates a function as printf-like for GCC/Clang's format-string static analysis.
 *  @param fmt  1-based parameter index of the format string argument.
 *  @param arg  1-based parameter index where the variadic arguments begin.
 *  Expands to nothing on non-GCC/Clang compilers (e.g. MSVC). */
#define LUA_PRINTF_ATTR(fmt, arg) __attribute__((format(printf, fmt, arg)))
#else
#define LUA_PRINTF_ATTR(fmt, arg)
#endif

/** @brief Marks a function as never returning to its caller.
 *  Maps to `__declspec(noreturn)` on MSVC and `__attribute__((__noreturn__))` elsewhere.
 *  Used on functions like luaD_throw that always longjmp or throw. */
#ifdef _MSC_VER
#define LUA_NORETURN __declspec(noreturn)
#else
#define LUA_NORETURN __attribute__((__noreturn__))
#endif

// Can be used to reconfigure visibility/exports for public APIs
#ifndef LUA_API
/** @brief Linkage/visibility specifier for the public C Lua API (e.g. lua_push*, lua_call).
 *  Defaults to `extern`.  Override before including lua.h to export symbols from a DLL
 *  (e.g. `#define LUA_API __declspec(dllexport)`). */
#define LUA_API extern
#endif

/** @brief Linkage/visibility specifier for the public Lua standard-library API.
 *  Defined as LUA_API — override LUA_API to change both simultaneously. */
#define LUALIB_API LUA_API

// Can be used to reconfigure visibility for internal APIs
#if defined(__GNUC__)
/** @brief Linkage specifier for internal (non-public) Luau functions.
 *  Uses GCC/Clang hidden visibility to keep symbols out of the dynamic symbol table,
 *  reducing link time and preventing accidental ABI exposure. */
#define LUAI_FUNC __attribute__((visibility("hidden"))) extern
/** @brief Linkage specifier for internal (non-public) Luau data symbols.
 *  Same hidden-visibility treatment as LUAI_FUNC. */
#define LUAI_DATA LUAI_FUNC
#else
/** @brief Linkage specifier for internal Luau functions (non-GCC fallback).
 *  Expands to plain `extern` — no visibility attribute available. */
#define LUAI_FUNC extern
/** @brief Linkage specifier for internal Luau data symbols (non-GCC fallback).
 *  Expands to plain `extern`. */
#define LUAI_DATA extern
#endif

// Can be used to reconfigure internal error handling to use longjmp instead of C++ EH
#ifndef LUA_USE_LONGJMP
/** @brief Selects the mechanism used for Lua error propagation.
 *  - `0` (default): use C++ exceptions (`throw`/`catch`).  Requires a C++ compiler and
 *    imposes unwind overhead, but interacts correctly with C++ destructors.
 *  - `1`: use `setjmp`/`longjmp`.  Avoids C++ EH machinery; useful for environments
 *    where exceptions are disabled (e.g. `-fno-exceptions`) or for C-only builds.
 *  Override before including luaconf.h. */
#define LUA_USE_LONGJMP 0
#endif

// LUA_IDSIZE gives the maximum size for the description of the source
#ifndef LUA_IDSIZE
/** @brief Maximum byte length (including NUL) of a source description string in debug info.
 *  Default: 256.  Appears in lua_Debug::short_src and error messages.
 *  Increase if source paths are very long; decrease to reduce per-Proto memory. */
#define LUA_IDSIZE 256
#endif

// LUA_MINSTACK is the guaranteed number of Lua stack slots available to a C function
#ifndef LUA_MINSTACK
/** @brief Minimum number of Lua stack slots guaranteed available when a C function is called.
 *  Default: 20.  Every C callback can safely use up to this many slots without calling
 *  lua_checkstack().  Raising this value increases per-call memory overhead. */
#define LUA_MINSTACK 20
#endif

// LUAI_MAXCSTACK limits the number of Lua stack slots that a C function can use
#ifndef LUAI_MAXCSTACK
/** @brief Hard upper bound on the number of Lua stack slots a C function may use.
 *  Default: 8000.  Attempting to grow the stack beyond this triggers a stack-overflow
 *  error.  Must be less than the OS native stack size divided by the TValue slot size. */
#define LUAI_MAXCSTACK 8000
#endif

// LUAI_MAXCALLS limits the number of nested calls
#ifndef LUAI_MAXCALLS
/** @brief Maximum total depth of nested Lua (and C) calls on a single thread.
 *  Default: 20000.  Counts both Lua-to-Lua and Lua-to-C frames.  Exceeding this limit
 *  raises a "stack overflow" error.  Keep well below LUAI_MAXCSTACK. */
#define LUAI_MAXCALLS 20000
#endif

// LUAI_MAXCCALLS is the maximum depth for nested C calls; this limit depends on native stack size
#ifndef LUAI_MAXCCALLS
/** @brief Maximum depth of nested C-to-C calls (C functions calling back into the VM).
 *  Default: 200.  This is deliberately much smaller than LUAI_MAXCALLS because each
 *  C frame also consumes native (OS) stack space.  Exceeding this raises an error. */
#define LUAI_MAXCCALLS 200
#endif

// buffer size used for on-stack string operations; this limit depends on native stack size
#ifndef LUA_BUFFERSIZE
/** @brief Size in bytes of the on-stack scratch buffer used by luaL_Strbuf and similar helpers.
 *  Default: 512.  Strings shorter than this avoid a heap allocation.  Increasing it
 *  reduces allocations but raises per-frame native stack usage. */
#define LUA_BUFFERSIZE 512
#endif

// number of valid Lua userdata tags
#ifndef LUA_UTAG_LIMIT
/** @brief Number of valid tagged-userdata type tags, in the range [0, LUA_UTAG_LIMIT-1].
 *  Default: 128.  Tags are set via lua_newuserdatatagged() and queried via lua_userdatatag().
 *  Increasing requires more entries in the per-state tag-metatable array. */
#define LUA_UTAG_LIMIT 128
#endif

// number of valid Lua lightuserdata tags
#ifndef LUA_LUTAG_LIMIT
/** @brief Number of valid lightuserdata type tags, in the range [0, LUA_LUTAG_LIMIT-1].
 *  Default: 128.  Lightuserdata tags allow the host to distinguish pointer types cheaply
 *  without allocating a full userdata object. */
#define LUA_LUTAG_LIMIT 128
#endif

// upper bound for number of size classes used by page allocator
#ifndef LUA_SIZECLASSES
/** @brief Upper bound on the number of size classes in the Luau page allocator (luaM).
 *  Default: 40.  Each size class gets its own free-list page pool.  Increasing this
 *  allows finer-grained bucketing of small allocations at the cost of more metadata. */
#define LUA_SIZECLASSES 40
#endif

// available number of separate memory categories
#ifndef LUA_MEMORY_CATEGORIES
/** @brief Number of independently tracked memory accounting categories.
 *  Default: 256.  Categories are indexed 0..N-1 and allow the host to attribute
 *  allocations to logical subsystems (e.g. scripts, UI, audio) for profiling. */
#define LUA_MEMORY_CATEGORIES 256
#endif

// extra storage for execution callbacks in global state
#ifndef LUA_EXECUTION_CALLBACK_STORAGE
/** @brief Bytes of extra inline storage reserved inside global_State for execution callbacks.
 *  Default: 512.  Used by the native code backend (codegen) to store per-state callback
 *  data without an extra heap allocation.  Must be large enough for the largest ecb payload. */
#define LUA_EXECUTION_CALLBACK_STORAGE 512
#endif

// minimum size for the string table (must be power of 2)
#ifndef LUA_MINSTRTABSIZE
/** @brief Minimum initial capacity of the global string-interning hash table.
 *  Default: 32.  MUST be a power of two — the table uses bitmask indexing.
 *  Increase for programs that intern many short-lived strings to reduce early rehashing. */
#define LUA_MINSTRTABSIZE 32
#endif

// maximum number of captures supported by pattern matching
#ifndef LUA_MAXCAPTURES
/** @brief Maximum number of capture groups allowed in a single string.find / string.match pattern.
 *  Default: 32.  Attempting more captures raises a pattern-error at runtime.
 *  Raising this increases the size of the on-stack capture array in lstrlib. */
#define LUA_MAXCAPTURES 32
#endif

// }==================================================================

#ifndef LUA_VECTOR_SIZE
/** @brief Number of components in a Luau vector value.
 *  Default: 3 (x, y, z).  Set to 4 to add a w component (x, y, z, w).
 *  MUST be either 3 or 4 — other values are not supported by the VM.
 *  Affects the TValue layout and the size of every vector on the stack/heap. */
#define LUA_VECTOR_SIZE 3 // must be 3 or 4
#endif

/** @brief Number of extra TValue slots needed to store the optional vector w component.
 *  Derived as `LUA_VECTOR_SIZE - 2`: 1 when LUA_VECTOR_SIZE == 3, 2 when == 4.
 *  Used internally to size TValue's union so vectors fit without additional allocation. */
#define LUA_EXTRA_SIZE (LUA_VECTOR_SIZE - 2)
