// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "lobject.h"
#include "ltm.h"

// registry
#define registry(L) (&L->global->registry)

// extra stack space to handle TM calls and some other extras
#define EXTRA_STACK 5

#define BASIC_CI_SIZE 8

#define BASIC_STACK_SIZE (2 * LUA_MINSTACK)

/*
** Global interned-string hash table.
** All TString objects that belong to the same global_State are stored here so that
** string equality can be checked with a pointer comparison.
*/
// clang-format off
typedef struct stringtable
{
    TString** hash; ///< pointer to the flat hash-bucket array (each bucket is a singly-linked list via TString::next)
    uint32_t nuse;  // number of elements
    int size;       ///< allocated number of buckets (always a power of two)
} stringtable;
// clang-format on

/*
** informations about a call
**
** the general Lua stack frame structure is as follows:
** - each function gets a stack frame, with function "registers" being stack slots on the frame
** - function arguments are associated with registers 0+
** - function locals and temporaries follow after; usually locals are a consecutive block per scope, and temporaries are allocated after this, but
*this is up to the compiler
**
** when function doesn't have varargs, the stack layout is as follows:
** ^ (func) ^^ [fixed args] [locals + temporaries]
** where ^ is the 'func' pointer in CallInfo struct, and ^^ is the 'base' pointer (which is what registers are relative to)
**
** when function *does* have varargs, the stack layout is more complex - the runtime has to copy the fixed arguments so that the 0+ addressing still
*works as follows:
** ^ (func) [fixed args] [varargs] ^^ [fixed args] [locals + temporaries]
**
** computing the sizes of these individual blocks works as follows:
** - the number of fixed args is always matching the `numparams` in a function's Proto object; runtime adds `nil` during the call execution as
*necessary
** - the number of variadic args can be computed by evaluating (ci->base - ci->func - 1 - numparams)
**
** the CallInfo structures are allocated as an array, with each subsequent call being *appended* to this array (so if f calls g, CallInfo for g
*immediately follows CallInfo for f)
** the `nresults` field in CallInfo is set by the caller to tell the function how many arguments the caller is expecting on the stack after the
*function returns
** the `flags` field in CallInfo contains internal execution flags that are important for pcall/etc, see LUA_CALLINFO_*
*/
// clang-format off
typedef struct CallInfo
{
    StkId base;    ///< register 0 of this frame — all Lua register accesses are relative to this pointer (base[reg])
    StkId func;    ///< stack slot that holds the Closure being executed; always one slot below base (or further back for vararg functions)
    StkId top;     ///< one past the last slot this frame may use; the VM asserts that L->top <= ci->top
    const Instruction* savedpc; ///< saved program counter; points into Proto::code and is updated on each call/return so the parent frame can resume

    int nresults;       // expected number of results from this function
    unsigned int flags; // call frame flags, see LUA_CALLINFO_*
} CallInfo;
// clang-format on

/// Set on the outermost CallInfo: when this frame returns the interpreter loop should exit rather than resuming a parent frame.
#define LUA_CALLINFO_RETURN (1 << 0) // should the interpreter return after returning from this callinfo? first frame must have this set
/// Set on a protected-call frame (pcall/xpcall): if an error is thrown inside, the runtime unwinds to this frame and invokes the C continuation
/// stored in the closure rather than propagating the error further.  The function at ci->func must be a C closure.
#define LUA_CALLINFO_HANDLE (1 << 1) // should the error thrown during execution get handled by continuation from this callinfo? func must be C
/// Set when this frame should be dispatched through lua_ExecutionCallbacks::enter instead of the bytecode interpreter.
/// Used by JIT/AOT backends to intercept execution of compiled functions.
#define LUA_CALLINFO_NATIVE (1 << 2) // should this function be executed using execution callback for native code

#define curr_func(L) (clvalue(L->ci->func))
#define ci_func(ci) (clvalue((ci)->func))
#define f_isLua(ci) (!ci_func(ci)->isC)
#define isLua(ci) (ttisfunction((ci)->func) && f_isLua(ci))

/*
** GC controller statistics used to tune the heap-trigger heuristic.
**
** The trigger is adjusted each GC cycle by a proportional-integral (PI) controller
** that tries to keep the heap growth ratio close to the configured goal.
*/
struct GCStats
{
    // data for proportional-integral controller of heap trigger value
    int32_t triggerterms[32] = {0};  ///< ring buffer of recent proportional error terms (heap-size deltas)
    uint32_t triggertermpos = 0;     ///< write cursor into the triggerterms ring buffer
    int32_t triggerintegral = 0;     ///< accumulated integral error term for the PI controller

    size_t atomicstarttotalsizebytes = 0; ///< total allocation size at the start of the atomic (stop-the-world) GC phase
    size_t endtotalsizebytes = 0;         ///< total allocation size at the end of the completed GC cycle
    size_t heapgoalsizebytes = 0;         ///< target heap size computed by the controller for the next trigger point

    double starttimestamp = 0;        ///< wall-clock time (seconds) when the current GC cycle began
    double atomicstarttimestamp = 0;  ///< wall-clock time when the atomic phase began
    double endtimestamp = 0;          ///< wall-clock time when the last GC cycle completed
};

#ifdef LUAI_GCMETRICS
/*
** Per-cycle GC timing and work metrics.  Populated when LUAI_GCMETRICS is defined.
** All `time` fields are in seconds; all `work` fields count abstract GC work units.
*/
struct GCCycleMetrics
{
    size_t starttotalsizebytes = 0;    ///< heap size in bytes at the start of this cycle
    size_t heaptriggersizebytes = 0;   ///< heap-trigger threshold that fired this cycle

    double pausetime = 0.0; // time from end of the last cycle to the start of a new one

    double starttimestamp = 0.0;  ///< wall-clock time when this cycle started
    double endtimestamp = 0.0;    ///< wall-clock time when this cycle ended

    double marktime = 0.0;           ///< total incremental mark time (seconds) across all steps
    double markassisttime = 0.0;     ///< mark time charged to mutator-assist steps
    double markmaxexplicittime = 0.0; ///< longest single explicit (non-assist) mark step
    size_t markexplicitsteps = 0;    ///< number of explicit mark steps taken
    size_t markwork = 0;             ///< total GC work units processed during marking

    double atomicstarttimestamp = 0.0;        ///< wall-clock time when the atomic phase started
    size_t atomicstarttotalsizebytes = 0;     ///< heap size at the start of the atomic phase
    double atomictime = 0.0;                  ///< total wall-clock time spent in the atomic phase

    // specific atomic stage parts
    double atomictimeupval = 0.0;  ///< time spent processing open upvalues during atomic
    double atomictimeweak = 0.0;   ///< time spent clearing weak table references during atomic
    double atomictimegray = 0.0;   ///< time spent draining the gray-again list during atomic
    double atomictimeclear = 0.0;  ///< time spent in the clear/finalize stage during atomic

    double sweeptime = 0.0;           ///< total incremental sweep time across all steps
    double sweepassisttime = 0.0;     ///< sweep time charged to mutator-assist steps
    double sweepmaxexplicittime = 0.0; ///< longest single explicit sweep step
    size_t sweepexplicitsteps = 0;    ///< number of explicit sweep steps taken
    size_t sweepwork = 0;             ///< total GC work units processed during sweeping

    size_t assistwork = 0;   ///< total work units performed by mutator-assist (all phases)
    size_t explicitwork = 0; ///< total work units performed by explicit GC steps (all phases)

    size_t propagatework = 0;      ///< work units done in the first mark propagation pass
    size_t propagateagainwork = 0; ///< work units done in the re-propagation pass (gray-again list)

    size_t endtotalsizebytes = 0; ///< heap size in bytes at the end of this cycle
};

struct GCMetrics
{
    double stepexplicittimeacc = 0.0; ///< cumulative wall-clock time of all explicit GC steps across all cycles
    double stepassisttimeacc = 0.0;   ///< cumulative wall-clock time of all mutator-assist steps across all cycles

    // when cycle is completed, last cycle values are updated
    uint64_t completedcycles = 0; ///< total number of full GC cycles completed since VM creation

    GCCycleMetrics lastcycle; ///< metrics snapshot of the most recently completed cycle
    GCCycleMetrics currcycle; ///< metrics being accumulated for the cycle currently in progress
};
#endif

/*
** Callbacks that can be used to to redirect code execution from Luau bytecode VM to a custom implementation (AoT/JiT/sandboxing/...)
**
** All callbacks are optional; a NULL pointer means "use default VM behaviour" for that hook.
** Set via lua_ExecutionCallbacks* lua_callbacks(L) and stored in global_State::ecb.
*/
struct lua_ExecutionCallbacks
{
    void* context; ///< opaque pointer passed to the backend; the VM does not read or write this field

    /**
     * Called when the global VM state (global_State) is being closed via lua_close().
     * Use this to release any resources the backend allocated for this VM instance.
     * @param L  the main thread of the VM being closed
     */
    void (*close)(lua_State* L);

    /**
     * Called just before a Proto is freed by the GC.
     * The backend should release any native code or metadata attached to proto->execdata / proto->exectarget.
     * @param L      any live lua_State (for allocator access)
     * @param proto  the Proto about to be freed; execdata is still valid at this point
     */
    void (*destroy)(lua_State* L, Proto* proto);

    /**
     * Called when a function with native execution data is about to start or resume.
     * Return 0 to hand control back to the bytecode interpreter; return non-zero to
     * indicate that the backend has fully executed the function and pushed its results.
     * This is the primary dispatch hook used by JIT/AOT backends.
     * @param L      current thread
     * @param proto  the Proto whose execdata / exectarget should be used
     * @return       0 → fall back to interpreter; non-zero → backend handled the call
     */
    int (*enter)(lua_State* L, Proto* proto);

    /**
     * Called when the debugger needs to switch a function back from native to bytecode
     * execution (e.g. to set a breakpoint).  The backend should invalidate any cached
     * native entry point so that subsequent calls go through the interpreter.
     * @param L      current thread
     * @param proto  the Proto to de-optimise
     */
    void (*disable)(lua_State* L, Proto* proto);

    /**
     * Called to query how many bytes of memory the backend has associated with a Proto's
     * native representation (for memory accounting / profiling).
     * @param L      current thread
     * @param proto  the Proto to query
     * @return       byte count of native memory; 0 if none
     */
    size_t (*getmemorysize)(lua_State* L, Proto* proto);

    /**
     * Called to map a userdata type-name string to an internal type index used by the
     * native type-specialisation system.
     * @param L    current thread
     * @param str  type name string
     * @param len  byte length of str
     * @return     type index byte; 0 means "unknown / no specialisation"
     */
    uint8_t (*gettypemapping)(lua_State* L, const char* str, size_t len);

    /**
     * Called to retrieve the execution counter buffer for a compiled Proto.
     * The buffer holds alternating pairs of {uint32_t call_count, uint32_t loop_iter, uint64_t ...}
     * entries that the backend uses for profile-guided optimisation.
     * @param L      current thread
     * @param proto  the Proto whose counters are requested
     * @param count  [out] number of counter entries in the returned array
     * @return       pointer to the counter data; NULL if unavailable
     */
    char* (*getcounterdata)(
        lua_State* L,
        Proto* proto,
        size_t* count
    ); // called to get the execution counter data and count {uint32_t, uint32_t, uint64_t}
};

/*
** `global state', shared by all threads of this state
*/
// clang-format off
typedef struct global_State
{
    stringtable strt; ///< global interned-string hash table; all live TString objects are registered here

    lua_Alloc frealloc;   // function to reallocate memory
    void* ud;             // auxiliary data to `frealloc'

    uint8_t currentwhite; ///< the GC-white colour bit that currently means "unreachable"; flips each cycle to avoid a full re-mark
    uint8_t gcstate;      // state of garbage collector

    GCObject* gray;      ///< list of gray objects waiting to be traversed (marked but children not yet visited)
    GCObject* grayagain; ///< list of objects that need a second traversal during the atomic phase (barriers wrote to them after initial mark)
    GCObject* weak;      ///< list of weak tables whose value/key slots must be cleared after the mark phase completes

    size_t GCthreshold;                       // when totalbytes >= GCthreshold, run GC step
    size_t totalbytes;                        ///< total bytes currently allocated across all memory categories

    int gcgoal;                               // see LUAI_GCGOAL
    int gcstepmul;                            // see LUAI_GCSTEPMUL
    int gcstepsize;                           // see LUAI_GCSTEPSIZE

    struct lua_Page* freepages[LUA_SIZECLASSES];    ///< per-size-class free-page lists for non-collectable (C) objects
    struct lua_Page* freegcopages[LUA_SIZECLASSES]; ///< per-size-class free-page lists for GC-collectable objects
    struct lua_Page* allpages;    // page linked list with all pages for all non-collectable object classes (available with LUAU_ASSERTENABLED)
    struct lua_Page* allgcopages; ///< page linked list containing every page used for GC-collectable objects; walked by the sweep phase
    struct lua_Page* sweepgcopage; // position of the sweep in `allgcopages'

    struct lua_State* mainthread; ///< the initial (main) coroutine created by lua_newstate(); always kept alive
    UpVal uvhead;                 ///< sentinel node of the global doubly-linked list of all open upvalues; real entries are between uvhead.u.open.prev and uvhead.u.open.next
    struct LuaTable* mt[LUA_T_COUNT]; ///< per-type metatables for primitive types (number, string, …); indexed by lua_Type
    TString* ttname[LUA_T_COUNT]; ///< interned type-name strings indexed by lua_Type (e.g. "number", "string", …)
    TString* tmname[TM_N];        ///< interned metamethod-name strings indexed by TMS enum (e.g. "__index", "__newindex", …)

    TValue pseudotemp; ///< scratch TValue used by pseudo2addr to materialise pseudo-indices; not GC-rooted, valid only during the C API call

    TValue registry;    ///< the Lua registry table (accessible via LUA_REGISTRYINDEX); the primary anchor for C-held Lua values
    int registryfree;   ///< index of the next free integer key in the registry (used by lua_ref)

    struct lua_jmpbuf* errorjmp; ///< current error-recovery jump buffer; set by lua_pcall/lua_resume and restored on error unwind

    uint64_t rngstate;    ///< PCG random number generator state; advanced by math.random
    uint64_t ptrenckey[4]; ///< 256-bit key used to obfuscate pointer values in tostring() output (prevents info-leak)

    lua_Callbacks cb;  ///< user-visible callbacks (lua_callbacks); interrupt, panic, userthread, etc.

    lua_ExecutionCallbacks ecb; ///< execution backend callbacks for JIT/AOT/sandboxing (see lua_ExecutionCallbacks)

    alignas(16) uint8_t ecbdata[LUA_EXECUTION_CALLBACK_STORAGE]; ///< inline storage block available to the execution backend (accessed via ecb.context or cast from this)

    size_t memcatbytes[LUA_MEMORY_CATEGORIES]; ///< per-category byte totals; index 0 is the default category; updated on every allocation

    void (*udatagc[LUA_UTAG_LIMIT])(lua_State*, void*); ///< per-tag __gc callbacks for full userdata; called immediately before the userdata memory is freed
    LuaTable* udatamt[LUA_UTAG_LIMIT]; ///< per-tag metatables for full userdata (indexed by Udata::tag)

    TString* lightuserdataname[LUA_LUTAG_LIMIT]; ///< optional type-name strings for tagged light userdata; used by typeof()

    GCStats gcstats; ///< PI-controller statistics used to compute the next GC trigger threshold

#ifdef LUAI_GCMETRICS
    GCMetrics gcmetrics; ///< detailed per-cycle timing and work metrics; only present when LUAI_GCMETRICS is defined
#endif
} global_State;
// clang-format on

/*
** `per thread' state
**
** lua_State represents a single Luau coroutine.  The main coroutine and every
** coroutine created with coroutine.create() has its own lua_State.  All coroutines
** in the same VM share the same global_State (accessible via L->global).
**
** lua_State IS a GCObject (CommonHeader is the first member) — coroutines are
** garbage collected when they are no longer reachable.
*/
// clang-format off
struct lua_State
{
    CommonHeader;
    uint8_t status; ///< coroutine status: LUA_OK (0) while running, LUA_YIELD when suspended, error codes on failure

    uint8_t activememcat; // memory category that is used for new GC object allocations

    bool isactive;   ///< true while this thread is actively executing; when true the GC may not move the stack without barriers
    bool singlestep; // call debugstep hook after each instruction

    StkId top;          ///< pointer to the first *free* stack slot (one past the last value); new values are pushed here
    StkId base;         ///< pointer to register 0 of the currently executing function (mirrors ci->base)
    global_State* global; ///< pointer to the shared VM state; all coroutines in the same lua_newstate() call share this
    CallInfo* ci;       ///< CallInfo for the currently executing function; points into the base_ci array
    StkId stack_last;   ///< one past the last usable stack slot; if top would exceed this, the stack must be grown
    StkId stack;        ///< base address of the value stack array (register 0 of the bottom-most frame is relative to this)

    CallInfo* end_ci;   ///< one past the last allocated CallInfo slot; if ci would exceed this, base_ci must be grown
    CallInfo* base_ci;  ///< base address of the CallInfo array; frames are appended sequentially

    int stacksize;      ///< total number of TValue slots currently allocated in the `stack` array
    int size_ci;                               // size of array `base_ci'

    unsigned short nCcalls;     ///< current depth of nested C calls (and C-called-Lua chains); guards against C stack overflow
    unsigned short baseCcalls;  ///< nCcalls value saved when a coroutine is resumed; restored on yield so the count stays consistent

    int cachedslot;    // when table operations or INDEX/NEWINDEX is invoked from Luau, what is the expected slot for lookup?

    LuaTable* gt;           ///< table of globals for this thread (usually the environment table set at thread creation)
    UpVal* openupval;       ///< head of the singly-linked list of open upvalues pointing into this thread's stack (via UpVal::u.open.threadnext)
    GCObject* gclist;       ///< intrusive GC grey/traversal list link

    TString* namecall; ///< when a NAMECALL instruction dispatches a method call into C, this holds the method name string until the C function reads it

    void* userdata; ///< arbitrary pointer for embedder use; not read or written by the VM
};
// clang-format on

/*
** Union of all collectible objects
**
** Every GC-managed heap allocation is one of these types.  The GCheader (gch)
** member provides type-safe access to the CommonHeader fields before the concrete
** type is known; the concrete type is then selected based on gch.tt.
*/
union GCObject
{
    GCheader gch;          ///< generic header view; valid for any GCObject — use gch.tt to identify the concrete type
    struct TString ts;     ///< LUA_TSTRING — interned immutable string
    struct Udata u;        ///< LUA_TUSERDATA — full (GC-managed) userdata block
    struct Closure cl;     ///< LUA_TFUNCTION — Lua or C closure
    struct LuaTable h;     ///< LUA_TTABLE — Lua table
    struct Proto p;        ///< LUA_TPROTO — compiled function prototype (not directly accessible from Lua)
    struct UpVal uv;       ///< LUA_TUPVAL — upvalue (internal; not directly accessible from Lua)
    struct lua_State th;   ///< LUA_TTHREAD — coroutine / thread
    struct LuauBuffer buf; ///< LUA_TBUFFER — mutable byte buffer
};

// macros to convert a GCObject into a specific value
#define gco2ts(o) check_exp((o)->gch.tt == LUA_TSTRING, &((o)->ts))
#define gco2u(o) check_exp((o)->gch.tt == LUA_TUSERDATA, &((o)->u))
#define gco2cl(o) check_exp((o)->gch.tt == LUA_TFUNCTION, &((o)->cl))
#define gco2h(o) check_exp((o)->gch.tt == LUA_TTABLE, &((o)->h))
#define gco2p(o) check_exp((o)->gch.tt == LUA_TPROTO, &((o)->p))
#define gco2uv(o) check_exp((o)->gch.tt == LUA_TUPVAL, &((o)->uv))
#define gco2th(o) check_exp((o)->gch.tt == LUA_TTHREAD, &((o)->th))
#define gco2buf(o) check_exp((o)->gch.tt == LUA_TBUFFER, &((o)->buf))

// macro to convert any Lua object into a GCObject
#define obj2gco(v) check_exp(iscollectable(v), cast_to(GCObject*, (v) + 0))

LUAI_FUNC lua_State* luaE_newthread(lua_State* L);
LUAI_FUNC void luaE_freethread(lua_State* L, lua_State* L1, struct lua_Page* page);
