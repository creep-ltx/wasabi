/*
 * patches.c - the code that hijacks system vectors, all of it, in one
 * place.
 *
 * Three features live here: the debug stream (exec's RawPutChar, so
 * every KPrintF byte), the snoop stream (thirty dos.library and
 * exec.library calls plus icon.library and diskfont), and the guru
 * report (exec's Alert, and the black box that outlives a reboot).
 * They are one file because they are one thing three times: patch a
 * vector, run in a stranger's context, drop data in a ring, let the
 * daemon drain it.
 *
 * THE RULES, once, for everything below the "foreign context" line in
 * each section. A patch stub runs in whatever task made the call - a
 * shell, Workbench, an interrupt, a task with 800 bytes of stack left
 * and a Forbid() held. So that code:
 *
 *   - allocates nothing, frees nothing, and opens nothing;
 *   - does no DOS I/O, and calls no function this file patches (that
 *     recurses without bound);
 *   - holds Disable() for a handful of instructions at most;
 *   - keeps its stack shallow, and checks the caller has room first;
 *   - reaches its globals by absolute address (no -fbaserel, so no a4
 *     to set up) and copies memory rather than trusting it;
 *   - calls only Signal(), which exec documents as interrupt-safe.
 *
 * And the rule that governs removal, which the afternoon of 13 August
 * 2026 paid for: a vector is only restored if it still points at us.
 * If somebody patched over the top, the stub must stay live and this
 * program must never unload - see patches_stuck().
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "patches.h"

extern struct ExecBase *SysBase;


/*
 * The debug stream, standalone: wasabid patches exec's RawPutChar (LVO
 * -516) itself and captures every KPrintF/serial-debug byte into a ring,
 * which the main loop drains to LOG frames. No Sashimi, no third-party
 * tool - the approach is modelled on Sashimi's public-domain source
 * (Olaf Barthel), which does the same SetFunction trick.
 *
 * The patch runs in ANY context - task, interrupt, Supervisor - because
 * that is where RawPutChar is called from. So the producer allocates
 * nothing, takes no locks beyond a short Disable(), does no I/O, and
 * (like Sashimi) does NOT chain to the original: serial output is
 * intercepted, not duplicated. Globals are absolute-addressed (no
 * -fbaserel), so the handler needs no a4 setup to reach the ring.
 */
#define DBG_RING  32768              /* power of two, for the & mask */

static volatile UBYTE g_ring[DBG_RING];
static volatile ULONG g_ring_head;   /* producer (patch) advances */
static volatile ULONG g_ring_tail;   /* consumer (daemon) advances */
static volatile ULONG g_ring_lost;   /* bytes dropped on a full ring */
static BOOL  g_dbg_patched;          /* our RawPutChar patch is installed */
static APTR  g_orig_rawput;          /* the vector we replaced */

/*
 * The SetFunction trampoline, in a top-level asm block so it is a real
 * linkable symbol. Entry: D0.b = char, A6 = ExecBase. Save the exec
 * scratch registers, hand the char to the C store on the stack, restore,
 * return. We do not call the original - matching Sashimi.
 */
extern void wasabi_rawput_patch(void);
void wasabi_store(UBYTE c);          /* forward decl; defined below */

asm(
    "    .text                       \n"
    "    .even                       \n"
    "    .globl _wasabi_rawput_patch \n"
    "_wasabi_rawput_patch:           \n"
    "    movem.l %d0-%d1/%a0-%a1,-(%sp) \n"
    "    move.l  %d0,-(%sp)          \n"   /* char as an int arg */
    "    jsr     _wasabi_store       \n"
    "    addq.l  #4,%sp              \n"
    "    movem.l (%sp)+,%d0-%d1/%a0-%a1 \n"
    "    rts                         \n"
);

void debug_inject(const char *s)
{
    while (*s)
        wasabi_store((UBYTE)*s++);
}

/* Producer. Runs in arbitrary context - keep it tiny and lock-light. */
void wasabi_store(UBYTE c)
{
    ULONG next;
    if (c == '\0')
        return;                      /* KPrintF pads with NULs; skip them */
    Disable();
    next = (g_ring_head + 1) & (DBG_RING - 1);
    if (next == g_ring_tail)
        g_ring_lost++;               /* full: drop, count it */
    else {
        g_ring[g_ring_head] = c;
        g_ring_head = next;
    }
    Enable();
}
static void debug_install(void)
{
    if (g_dbg_patched)
        return;
    /* Disable across SetFunction: RawPutChar can fire from interrupts. */
    Disable();
    g_orig_rawput = SetFunction((struct Library *)SysBase, -516,
                                (ULONG (*)())wasabi_rawput_patch);
    Enable();
    g_dbg_patched = TRUE;
}

/*
 * Remove the patch, but only if the vector still points at ours. If
 * someone SetFunction'd on top of us, restoring the old pointer would
 * unlink THEIR patch and crash the machine later - so we put ours back
 * and stay installed rather than corrupt the chain. (Same rule Sashimi
 * uses; the honest failure is a patch that will not leave, not a Guru
 * with no traceable cause.)
 */
static BOOL debug_uninstall(void)
{
    APTR res;
    BOOL removed;
    if (!g_dbg_patched)
        return TRUE;
    Disable();
    res = SetFunction((struct Library *)SysBase, -516,
                      (ULONG (*)())g_orig_rawput);
    if (res == (APTR)wasabi_rawput_patch) {
        removed = TRUE;
    } else {
        /* Someone is on top of us - undo our restore, leave the stack. */
        SetFunction((struct Library *)SysBase, -516, (ULONG (*)())res);
        removed = FALSE;
    }
    Enable();
    if (removed)
        g_dbg_patched = FALSE;
    return removed;
}

/* Consumer side: pull bytes the patch has queued. Single consumer, so no
 * Disable needed - we only read head and advance tail. */
static LONG ring_drain(UBYTE *buf, LONG max)
{
    ULONG head = g_ring_head;            /* one atomic snapshot */
    LONG n = 0;
    while (n < max && g_ring_tail != head) {
        buf[n++] = g_ring[g_ring_tail];
        g_ring_tail = (g_ring_tail + 1) & (DBG_RING - 1);
    }
    return n;
}

/* Defined with the guru report below; both stream starts replay it. */
void debug_start(void)
{
    g_ring_head = g_ring_tail = g_ring_lost = 0;
    debug_install();
}

LONG debug_drain(UBYTE *buf, LONG max)
{
    return ring_drain(buf, max);
}

ULONG debug_take_lost(void)
{
    ULONG n;
    Disable();                       /* the producer increments this from
                                      * any context; a plain read-then-
                                      * clear silently drops what lands
                                      * between the two */
    n = g_ring_lost;
    g_ring_lost = 0;
    Enable();
    return n;
}

void debug_stop(void)
{
    debug_uninstall();
}

/* --- the snoop stream ---------------------------------------------- */

/*
 * The SnoopDOS trick, after Eddy Carroll's public source: SetFunction()
 * patches on the dos.library and exec.library calls a developer most
 * wants to see. Unlike SnoopDOS we log AFTER the original returns, so
 * every line carries the real result and IoErr() instead of "pending".
 *
 * Each patch is a two-instruction stub that pushes its descriptor and
 * jumps to one common trampoline. The trampoline runs in the CALLER's
 * context - any task on the machine - so the same rules as the debug
 * patch apply, plus one more: nothing reachable from here may call a
 * function we patch, or it recurses without bound. The C side therefore
 * reads SysBase->ThisTask directly and only ever copies memory.
 *
 * Three ideas are borrowed straight from SnoopDOS's patchcode.s:
 *  - the stub tests an enabled flag first and falls through to the
 *    original untouched when snooping is off, so a patch that cannot be
 *    removed (someone chained onto it) idles at a few instructions;
 *  - a use count is bumped while a caller is inside the stub, so the
 *    daemon can wait for stragglers before its code segment unloads;
 *  - events are dropped, and counted, when the calling task is low on
 *    stack rather than overflowing it building the record.
 */

/* SNOOP's flags word, reserved since the first protocol version. */
#define SNOOPF_ENTRY 1               /* log calls on the way in as well */

#define SN_EVMAX   512               /* events in the ring, power of two.
                                      * ~107 KB of BSS: a PiStorm-fast CPU
                                      * can fire >150 patched calls between
                                      * two 50 ms drains (List SYS:C did),
                                      * and 64 dropped most of that burst */
#define SN_NONE    0xFF              /* "no such argument" register index */

/* Register-file indices: the trampoline saves d0-d7/a0-a6 in order. */
#define RF_D0 0
#define RF_D1 1
#define RF_D2 2
#define RF_D3 3
#define RF_D4 4
#define RF_A0 8
#define RF_A1 9

#define SN_DOS      0                /* which library the LVO lives in */
#define SN_EXEC     1
#define SN_ICON     2                /* opened by us while snooping */
#define SN_DISKFONT 3
#define SN_NLIB     4

#define NK_NONE     0                /* how to render the numeric arg */
#define NK_OPENMODE 1
#define NK_LOCKMODE 2
#define NK_VERSION  3
#define NK_UNIT     4
#define NK_BPTR     5                /* a lock, shown as a raw BPTR */
#define NK_HEX      6                /* a mask (SetProtection) */
#define NK_PLAIN    7                /* just a number (RunCommand's length) */

#define RK_BOOL 0                    /* how to read d0: nonzero = ok */
#define RK_RC   1                    /* a return code (SystemTagList) */
#define RK_LEN  2                    /* -1 = fail, else a length (GetVar) */
#define RK_ZERO 3                    /* zero = ok (OpenDevice) */
#define RK_PTR  4                    /* the value itself is the answer */

#define SNF_SOFT2 1                  /* str2 is a string only if d3 != 0 */
#define SNF_DEREF1 2                 /* str1 register points AT the string
                                      * pointer, not at the string: a
                                      * TextAttr's ta_Name is its first
                                      * field, which is how OpenDiskFont
                                      * gets a readable name */
#define SNF_LEN1  4                  /* str1 is not NUL-terminated - the
                                      * numeric arg is its length, and the
                                      * copy stops there (RunCommand) */

struct SnoopFn {
    APTR        orig;                /* MUST be first: the asm reads (a2) */
    const char *name;
    APTR        stub;
    UBYTE       lib;                 /* SN_DOS / SN_EXEC */
    WORD        lvo;
    UBYTE       str1, str2;          /* register-file index of string args */
    UBYTE       numreg;              /* index of the numeric arg */
    UBYTE       nkind, rkind, flags;
    UBYTE       installed;
};

struct SnoopEv {
    struct SnoopFn *fn;
    char  task[32];
    char  s1[100];
    char  s2[64];
    BOOL  has2;
    BOOL  entry;                     /* logged on the way IN: no result yet */
    LONG  num;
    LONG  res;
    LONG  err;                       /* pr_Result2 after the call */
};

static struct SnoopEv g_snev[SN_EVMAX];
static volatile ULONG g_snev_head;   /* producer (patches) advances */
static volatile ULONG g_snev_tail;   /* consumer (daemon) advances */
static volatile ULONG g_snev_lost;

static char         g_snoop_pat[64];
static struct Task *g_snoop_self;    /* the daemon; its own calls are not
                                      * logged, or draining the ring over
                                      * bsdsocket could feed the ring */

/* icon.library and diskfont.library are not open on a bare system, and
 * a library with our patch in it must not be expunged underneath us -
 * so snoop holds them open for exactly as long as it is patched. NULL
 * means "could not open": those descriptors are skipped, not fatal. */
static struct Library *g_snoop_libs[SN_NLIB];

/* The daemon itself: entry-mode snooping and the Alert patch both wake
 * it from another task's context. Set once, in main(). */
static struct Task *g_daemon_task;

/* Read by the trampoline, so plain globals with asm-visible names. */
volatile UWORD wasabi_snoop_on;      /* stubs record only while set */
volatile UWORD wasabi_snoop_users;   /* callers currently inside a stub */
volatile UWORD wasabi_snoop_entry;   /* also log calls on the way in */

void snoop_record(struct SnoopFn *d, ULONG *regs, LONG res);
void snoop_record_entry(struct SnoopFn *d, ULONG *regs);

/*
 * HERE BE DRAGONS. This trampoline and the RF_* indices in the
 * descriptors above are one definition split in two: the movem below
 * decides what the 60-byte register file looks like, and the indices
 * decide where an argument is read from inside it. No compiler checks
 * that they agree. Get it wrong and copystr() dereferences
 * whatever was in the wrong register, in another task's context, on a
 * machine with no memory protection.
 *
 * Change nothing here without running snoop_selftest() on real
 * hardware - it exists precisely to catch this, and `wasabi snoop`
 * runs it before every session.
 *
 * The common trampoline. Entered from a per-function stub that has just
 * pushed its descriptor, so the stack is [desc][caller RA] and every
 * argument register still holds the caller's value.
 *
 * Off: put the original's address where the descriptor was and rts into
 * it, clobbering nothing (the a0 juggle is because 68000 has no
 * memory-to-memory move through a pointer).
 *
 * On: bump the use count, save the full register file, call the original
 * with the caller's registers intact, hand descriptor + register file +
 * result to C, then restore with d0 (and d1 - old dos.library returned
 * results in both, and SnoopDOS keeps that quirk alive) as the result.
 *
 * Entry logging sits between the register file and that call. It is
 * gated on a plain global word - the same trick the enabled check
 * already uses, and deliberately NOT on a field of the descriptor:
 * a struct offset hardcoded in asm is exactly the split definition
 * this comment warns about. Since the C call clobbers the scratch
 * registers the original still needs, the whole file is reloaded
 * afterwards and a2/a3 re-derived from the stack, which leaves the
 * machine in precisely the state the no-entry path reaches the jsr in.
 */
asm(
    "    .text                          \n"
    "    .even                          \n"
    "    .globl _wasabi_snoop_common    \n"
    "_wasabi_snoop_common:              \n"
    "    tst.w   _wasabi_snoop_on       \n"
    "    bne     snoop_live             \n"
    "    move.l  %a0,-(%sp)             \n"  /* [a0][desc][RA] */
    "    move.l  4(%sp),%a0             \n"  /* a0 = desc */
    "    move.l  (%a0),%a0              \n"  /* a0 = desc->orig */
    "    move.l  %a0,4(%sp)             \n"  /* [a0][orig][RA] */
    "    move.l  (%sp)+,%a0             \n"  /* [orig][RA] */
    "    rts                            \n"  /* jump to the original */
    "snoop_live:                        \n"
    "    addq.w  #1,_wasabi_snoop_users \n"
    "    movem.l %d0-%d7/%a0-%a6,-(%sp) \n"  /* the 60-byte register file */
    "    tst.w   _wasabi_snoop_entry    \n"
    "    beq     snoop_docall           \n"
    "    move.l  60(%sp),%a2            \n"  /* a2 = desc */
    "    pea     0(%sp)                 \n"  /* C arg 2: the register file */
    "    move.l  %a2,-(%sp)             \n"  /* C arg 1: the descriptor */
    "    jsr     _snoop_record_entry    \n"
    "    addq.l  #8,%sp                 \n"
    "    movem.l (%sp),%d0-%d7/%a0-%a6  \n"  /* every caller register back */
    "snoop_docall:                      \n"
    "    move.l  60(%sp),%a2            \n"  /* a2 = desc */
    "    move.l  (%a2),%a3              \n"  /* a3 = desc->orig */
    "    jsr     (%a3)                  \n"  /* argument regs untouched */
    "    move.l  %d0,-(%sp)             \n"  /* C arg 3: the result */
    "    pea     4(%sp)                 \n"  /* C arg 2: the register file */
    "    move.l  %a2,-(%sp)             \n"  /* C arg 1: the descriptor */
    "    jsr     _snoop_record          \n"
    "    addq.l  #8,%sp                 \n"
    "    move.l  (%sp)+,%d0             \n"  /* result back... */
    "    move.l  %d0,(%sp)              \n"  /* ...into the file's d0 slot */
    "    movem.l (%sp)+,%d0-%d7/%a0-%a6 \n"
    "    addq.l  #4,%sp                 \n"  /* drop the descriptor */
    "    subq.w  #1,_wasabi_snoop_users \n"
    "    move.l  %d0,%d1                \n"
    "    rts                            \n"
);

#define SNOOP_DESC(nm, lib, lvo, s1, s2, nreg, nk, rk, fl)                 \
extern void snoop_stub_##nm(void);                                         \
asm("    .text                          \n"                                \
    "    .even                          \n"                                \
    "    .globl _snoop_stub_" #nm "     \n"                                \
    "_snoop_stub_" #nm ":               \n"                                \
    "    pea     _snoop_desc_" #nm "    \n"                                \
    "    jmp     _wasabi_snoop_common   \n");                              \
struct SnoopFn snoop_desc_##nm = {                                         \
    NULL, #nm, (APTR)snoop_stub_##nm, lib, lvo, s1, s2, nreg, nk, rk, fl, \
    FALSE };

/* LVOs verified against the NDK's dos_lib.fd / exec_lib.fd. */
SNOOP_DESC(Open,          SN_DOS,  -30,  RF_D1, SN_NONE, RF_D2,
           NK_OPENMODE, RK_BOOL, 0)
SNOOP_DESC(Lock,          SN_DOS,  -84,  RF_D1, SN_NONE, RF_D2,
           NK_LOCKMODE, RK_BOOL, 0)
SNOOP_DESC(LoadSeg,       SN_DOS,  -150, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(Execute,       SN_DOS,  -222, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(SystemTagList, SN_DOS,  -606, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_RC, 0)
SNOOP_DESC(GetVar,        SN_DOS,  -906, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_LEN, 0)
SNOOP_DESC(SetVar,        SN_DOS,  -900, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(DeleteFile,    SN_DOS,  -72,  RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(Rename,        SN_DOS,  -78,  RF_D1, RF_D2, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(CreateDir,     SN_DOS,  -120, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(MakeLink,      SN_DOS,  -444, RF_D1, RF_D2, SN_NONE,
           NK_NONE, RK_BOOL, SNF_SOFT2)
SNOOP_DESC(OpenLibrary,   SN_EXEC, -552, RF_A1, SN_NONE, RF_D0,
           NK_VERSION, RK_BOOL, 0)
SNOOP_DESC(OpenDevice,    SN_EXEC, -444, RF_A0, SN_NONE, RF_D0,
           NK_UNIT, RK_ZERO, 0)
SNOOP_DESC(FindPort,      SN_EXEC, -390, RF_A1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)

/* The rest of SnoopDOS's list. CurrentDir's lock stays a raw BPTR:
 * naming it would mean NameFromLock, a dos.library call, from inside a
 * dos.library patch in someone else's context. RunCommand's parameter
 * string is counted, not terminated, so its copy is bounded by d4. */
SNOOP_DESC(CurrentDir,    SN_DOS,  -126, SN_NONE, SN_NONE, RF_D1,
           NK_BPTR, RK_PTR, 0)
SNOOP_DESC(SetComment,    SN_DOS,  -180, RF_D1, RF_D2, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(SetProtection, SN_DOS,  -186, RF_D1, SN_NONE, RF_D2,
           NK_HEX, RK_BOOL, 0)
SNOOP_DESC(RunCommand,    SN_DOS,  -504, RF_D3, SN_NONE, RF_D4,
           NK_PLAIN, RK_RC, SNF_LEN1)
SNOOP_DESC(NewLoadSeg,    SN_DOS,  -768, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(DeleteVar,     SN_DOS,  -912, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(FindResident,  SN_EXEC, -96,  RF_A1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(FindTask,      SN_EXEC, -294, RF_A1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(OpenResource,  SN_EXEC, -498, RF_A1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(FindSemaphore, SN_EXEC, -594, RF_A1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(GetDiskObject, SN_ICON, -78,  RF_A0, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(PutDiskObject, SN_ICON, -84,  RF_A0, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(FindToolType,  SN_ICON, -96,  RF_A1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(MatchToolValue, SN_ICON, -102, RF_A0, RF_A1, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(GetDiskObjectNew, SN_ICON, -132, RF_A0, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(OpenDiskFont,  SN_DISKFONT, -30, RF_A0, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, SNF_DEREF1)

static struct SnoopFn *snoop_fns[] = {
    &snoop_desc_Open,        &snoop_desc_Lock,      &snoop_desc_LoadSeg,
    &snoop_desc_Execute,     &snoop_desc_SystemTagList,
    &snoop_desc_GetVar,      &snoop_desc_SetVar,    &snoop_desc_DeleteFile,
    &snoop_desc_Rename,      &snoop_desc_CreateDir, &snoop_desc_MakeLink,
    &snoop_desc_OpenLibrary, &snoop_desc_OpenDevice, &snoop_desc_FindPort,
    &snoop_desc_CurrentDir,  &snoop_desc_SetComment,
    &snoop_desc_SetProtection, &snoop_desc_RunCommand,
    &snoop_desc_NewLoadSeg,  &snoop_desc_DeleteVar,
    &snoop_desc_FindResident, &snoop_desc_FindTask,
    &snoop_desc_OpenResource, &snoop_desc_FindSemaphore,
    &snoop_desc_GetDiskObject, &snoop_desc_PutDiskObject,
    &snoop_desc_FindToolType, &snoop_desc_MatchToolValue,
    &snoop_desc_GetDiskObjectNew, &snoop_desc_OpenDiskFont,
};
#define SN_NFN (LONG)(sizeof(snoop_fns) / sizeof(snoop_fns[0]))

/* strncpy that always terminates and tolerates a NULL source. */
void copystr(char *dst, LONG dstsz, const char *src)
{
    LONG i = 0;
    if (!src) src = "(null)";
    while (i < dstsz - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Bounded copy of a counted (not NUL-terminated) string: RunCommand's
 * parameter block is d4 bytes long and often ends in a newline rather
 * than a zero, so trust the count, not the bytes. */
static void snoop_copycount(char *dst, LONG dstsz, const char *src, LONG len)
{
    LONG i = 0;
    if (!src || len < 0) { copystr(dst, dstsz, src); return; }
    while (i < dstsz - 1 && i < len && src[i] && src[i] != '\n') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* The expensive half, in its own frame so the headroom check in
 * snoop_record() has already passed before this local exists. */
static void __attribute__((noinline))
snoop_record2(struct SnoopFn *d, ULONG *regs, LONG res, BOOL entry)
{
    struct SnoopEv ev;
    struct Task *t = SysBase->ThisTask;
    ULONG next;

    ev.fn   = d;
    ev.num  = (d->numreg != SN_NONE) ? (LONG)regs[d->numreg] : 0;
    ev.res  = res;
    ev.err  = 0;
    ev.has2 = FALSE;
    ev.entry = entry;
    ev.task[0] = '\0';

    /* Name the culprit the way SnoopDOS does: the CLI command being run
     * if there is one, else the task name. */
    if (t->tc_Node.ln_Type == NT_PROCESS) {
        struct Process *pr = (struct Process *)t;
        struct CommandLineInterface *cli =
            (struct CommandLineInterface *)BADDR(pr->pr_CLI);
        if (!entry)                      /* on the way in, pr_Result2 still
                                          * belongs to some earlier call */
            ev.err = pr->pr_Result2;
        if (cli) {
            UBYTE *b = (UBYTE *)BADDR(cli->cli_CommandName);
            if (b && b[0]) {                 /* a BSTR: length, then bytes */
                LONG n = b[0];
                if (n > (LONG)sizeof(ev.task) - 1)
                    n = sizeof(ev.task) - 1;
                memcpy(ev.task, b + 1, n);
                ev.task[n] = '\0';
            }
        }
    }
    if (!ev.task[0])
        copystr(ev.task, sizeof(ev.task), t->tc_Node.ln_Name);

    if (d->str1 != SN_NONE) {
        ULONG p = regs[d->str1];
        /* A TextAttr, not a string: its first field is the name. One
         * indirection, guarded - a caller's struct pointer is as
         * trustworthy as the string pointers we already follow. */
        if ((d->flags & SNF_DEREF1) && p)
            p = *(ULONG *)p;
        if (d->flags & SNF_LEN1)
            snoop_copycount(ev.s1, sizeof(ev.s1), (const char *)p, ev.num);
        else
            copystr(ev.s1, sizeof(ev.s1), (const char *)p);
    } else
        ev.s1[0] = '\0';
    if (d->str2 != SN_NONE) {
        ev.has2 = TRUE;
        if ((d->flags & SNF_SOFT2) && !regs[RF_D3])
            strcpy(ev.s2, "(hard link to a lock)");
        else
            copystr(ev.s2, sizeof(ev.s2), (const char *)regs[d->str2]);
    } else
        ev.s2[0] = '\0';

    Disable();
    next = (g_snev_head + 1) & (SN_EVMAX - 1);
    if (next == g_snev_tail)
        g_snev_lost++;                       /* full: drop, count it */
    else {
        g_snev[g_snev_head] = ev;
        g_snev_head = next;
    }
    Enable();
}

/*
 * Producer half, called from the trampoline in the context of whatever
 * task made the call - after the original has already returned.
 */
void snoop_record(struct SnoopFn *d, ULONG *regs, LONG res)
{
    struct Task *t = SysBase->ThisTask;
    char probe;

    if (!wasabi_snoop_on || t == g_snoop_self)
        return;
    /* SnoopDOS's stack rule: if sp is inside the task's declared stack,
     * insist on headroom before building the event on it; an sp outside
     * the bounds (CLI programs swap stacks) is assumed to be roomy. */
    if ((APTR)&probe > t->tc_SPLower && (APTR)&probe <= t->tc_SPUpper &&
        (char *)&probe - (char *)t->tc_SPLower < 800) {
        Disable(); g_snev_lost++; Enable();
        return;
    }
    snoop_record2(d, regs, res, FALSE);
}

/*
 * The same, on the way IN - the only way to see a call that never
 * comes back. A machine that dies inside brcm-emmc.device leaves its
 * last completed call in the log and its fatal one nowhere; this puts
 * the fatal one on the wire before it is made.
 *
 * Racing a freeze is the whole point, so the daemon is signalled the
 * moment the event is queued rather than waiting up to a poll interval
 * for the drain. That is a task switch per patched call - entry mode
 * is deliberately slow, and deliberately opt-in.
 */
void snoop_record_entry(struct SnoopFn *d, ULONG *regs)
{
    struct Task *t = SysBase->ThisTask;
    char probe;

    if (!wasabi_snoop_on || !wasabi_snoop_entry || t == g_snoop_self)
        return;
    if ((APTR)&probe > t->tc_SPLower && (APTR)&probe <= t->tc_SPUpper &&
        (char *)&probe - (char *)t->tc_SPLower < 800) {
        Disable(); g_snev_lost++; Enable();
        return;
    }
    snoop_record2(d, regs, 0, TRUE);
    if (g_daemon_task && t != g_daemon_task)
        Signal(g_daemon_task, SIGBREAKF_CTRL_F);
}

static struct Library *snoop_base(struct SnoopFn *d)
{
    switch (d->lib) {
    case SN_EXEC: return (struct Library *)SysBase;
    case SN_DOS:  return (struct Library *)DOSBase;
    default:      return g_snoop_libs[d->lib];
    }
}

/*
 * icon.library and diskfont.library get opened here and closed only
 * when every patch of theirs is out again. That open is not politeness
 * - a library expunged with our stub in its jump table takes the stub
 * with it, and the restore afterwards would write a stale pointer into
 * a fresh library. Holding it open makes expunge impossible.
 */
static void snoop_openlibs(void)
{
    if (!g_snoop_libs[SN_ICON])
        g_snoop_libs[SN_ICON] = OpenLibrary("icon.library", 0);
    if (!g_snoop_libs[SN_DISKFONT])
        g_snoop_libs[SN_DISKFONT] = OpenLibrary("diskfont.library", 0);
}

static void snoop_closelibs(void)
{
    LONG i;
    for (i = 0; i < SN_NFN; i++)     /* a stuck patch keeps its base */
        if (snoop_fns[i]->installed && snoop_fns[i]->lib >= SN_ICON)
            return;
    for (i = SN_ICON; i < SN_NLIB; i++) {
        if (g_snoop_libs[i])
            CloseLibrary(g_snoop_libs[i]);
        g_snoop_libs[i] = NULL;
    }
}

static void snoop_install(void)
{
    LONG i;
    snoop_openlibs();
    for (i = 0; i < SN_NFN; i++) {
        struct SnoopFn *d = snoop_fns[i];
        if (d->installed)
            continue;                /* left over from a stuck teardown */
        if (!snoop_base(d))
            continue;                /* that library is not on this machine */
        Disable();
        d->orig = (APTR)SetFunction(snoop_base(d), d->lvo,
                                    (ULONG (*)())d->stub);
        Enable();
        d->installed = TRUE;
    }
}

/* Reverse order, same chain rule as the debug patch: if the vector no
 * longer points at us, put the interloper back and stay installed. */
static BOOL snoop_uninstall(void)
{
    LONG i;
    BOOL all = TRUE;
    for (i = SN_NFN - 1; i >= 0; i--) {
        struct SnoopFn *d = snoop_fns[i];
        APTR res;
        if (!d->installed)
            continue;
        Disable();
        res = (APTR)SetFunction(snoop_base(d), d->lvo, (ULONG (*)())d->orig);
        if (res == d->stub)
            d->installed = FALSE;
        else {
            SetFunction(snoop_base(d), d->lvo, (ULONG (*)())res);
            all = FALSE;             /* the stub stays live; its enabled
                                      * check makes it a cheap no-op */
        }
        Enable();
    }
    /*
     * Deliberately NOT closing icon.library/diskfont.library here. A
     * caller can still be parked inside our stub - or inside the
     * library itself - and dropping the open count to zero invites an
     * expunge under its feet. They are released by patches_closelibs()
     * at the very end of teardown, after the straggler wait.
     */
    return all;
}

static BOOL snoop_stuck(void)
{
    LONG i;
    for (i = 0; i < SN_NFN; i++)
        if (snoop_fns[i]->installed)
            return TRUE;
    return FALSE;
}

/*
 * Case-insensitive match with the AmigaDOS '#?' and '?' wildcards ('*'
 * too, since fingers type it). Not full ParsePattern - but this runs on
 * task names, not paths, and needs no dos.library call.
 */
static BOOL pat_match(const char *pat, const char *s)
{
    while (*pat) {
        if ((pat[0] == '#' && pat[1] == '?') || pat[0] == '*') {
            const char *rest = pat + (pat[0] == '*' ? 1 : 2);
            for (;; s++) {
                if (pat_match(rest, s))
                    return TRUE;
                if (!*s)
                    return FALSE;
            }
        }
        if (!*s)
            return FALSE;
        if (pat[0] != '?' &&
            tolower((unsigned char)pat[0]) != tolower((unsigned char)s[0]))
            return FALSE;
        pat++; s++;
    }
    return *s == '\0';
}

/* Daemon context from here down. */

static LONG snoop_format(struct SnoopEv *ev, char *out)
{
    struct SnoopFn *d = ev->fn;
    LONG n = sprintf(out, "%-20s %s(", ev->task, d->name);

    if (d->str1 != SN_NONE)
        n += sprintf(out + n, "\"%s\"", ev->s1);
    if (ev->has2)
        n += sprintf(out + n, ", \"%s\"", ev->s2);

    switch (d->nkind) {
    case NK_OPENMODE:
        if (ev->num == 1005)      n += sprintf(out + n, ", read");
        else if (ev->num == 1006) n += sprintf(out + n, ", create");
        else if (ev->num == 1004) n += sprintf(out + n, ", readwrite");
        else n += sprintf(out + n, ", mode %ld", (long)ev->num);
        break;
    case NK_LOCKMODE:
        if (ev->num == -2)        n += sprintf(out + n, ", read");
        else if (ev->num == -1)   n += sprintf(out + n, ", write");
        else n += sprintf(out + n, ", type %ld", (long)ev->num);
        break;
    case NK_VERSION:
        n += sprintf(out + n, ", v%ld", (long)ev->num);
        break;
    case NK_UNIT:
        n += sprintf(out + n, ", unit %ld", (long)ev->num);
        break;
    case NK_BPTR:
        n += sprintf(out + n, "lock 0x%08lx", (unsigned long)ev->num);
        break;
    case NK_HEX:
        n += sprintf(out + n, ", 0x%08lx", (unsigned long)ev->num);
        break;
    case NK_PLAIN:
        n += sprintf(out + n, ", %ld byte(s)", (long)ev->num);
        break;
    }

    /* On the way in there is no result yet - and saying so is the
     * point: a line that never gains its partner is the call the
     * machine died inside. */
    if (ev->entry)
        return n + sprintf(out + n, ") ...\n");

    switch (d->rkind) {
    case RK_RC:
        n += sprintf(out + n, ") = rc %ld\n", (long)ev->res);
        break;
    case RK_LEN:
        if (ev->res >= 0)
            n += sprintf(out + n, ") = %ld byte(s)\n", (long)ev->res);
        else if (ev->err)
            n += sprintf(out + n, ") = fail (err %ld)\n", (long)ev->err);
        else
            n += sprintf(out + n, ") = fail\n");
        break;
    case RK_ZERO:
        if (ev->res == 0)
            n += sprintf(out + n, ") = ok\n");
        else
            n += sprintf(out + n, ") = error %ld\n", (long)ev->res);
        break;
    case RK_PTR:
        n += sprintf(out + n, ") = 0x%08lx\n", (unsigned long)ev->res);
        break;
    default:                         /* RK_BOOL */
        if (ev->res)
            n += sprintf(out + n, ") = ok\n");
        else if (ev->err && (d->lib == SN_DOS || d->lib == SN_ICON))
            n += sprintf(out + n, ") = fail (err %ld)\n", (long)ev->err);
        else
            n += sprintf(out + n, ") = fail\n");
        break;
    }
    return n;
}

/*
 * Prove the trampoline before trusting it.
 *
 * The asm hands snoop_record() a 60-byte register file; the RF_* indices
 * in each descriptor say where in it an argument lives. Those are two
 * definitions that must agree exactly, and nothing at compile time
 * checks that they do. The failure mode is not a wrong log line - it is
 * a wild pointer copied out of the wrong register and dereferenced by
 * copystr(), on a machine with no MMU, in the context of whatever
 * task happened to call Lock().
 *
 * So before any of that can happen: un-exclude our own calls, Lock() a
 * bogus path, and insist the captured event carries back that exact
 * string (d1), the mode we passed (d2) and the result the caller
 * actually got (d0). A mismatch means the asm and the C disagree on
 * THIS build, and snoop refuses to run rather than corrupt memory three
 * minutes later somewhere that looks nothing like the cause.
 *
 * The path is relative - no device lookup, so a missing object cannot
 * raise a "please insert volume" requester - and pr_WindowPtr is -1 for
 * the duration in case anything else would try.
 */
static BOOL snoop_selftest(char *why, LONG whysz, BOOL entry)
{
    static LONG serial;
    char path[64];
    struct Task *self = FindTask(NULL);
    struct Process *me = (struct Process *)self;
    BOOL isproc = self->tc_Node.ln_Type == NT_PROCESS;
    APTR oldwin = NULL;
    BPTR lock;
    ULONG i;
    BOOL found = FALSE;
    BOOL found_entry = FALSE;
    BOOL found_a = FALSE;            /* the A-register half of the file */

    /* Unique per attempt, so a stale ring entry can never be mistaken
     * for this run's event. */
    sprintf(path, "wasabi-selftest-%ld-%lx", (long)++serial,
            (unsigned long)self);

    g_snev_head = g_snev_tail = g_snev_lost = 0;
    g_snoop_self = NULL;             /* record our own call, just this once */
    wasabi_snoop_entry = entry ? 1 : 0;
    wasabi_snoop_on = 1;

    if (isproc) {
        oldwin = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;
    }
    lock = Lock(path, ACCESS_READ);  /* must fail; nothing to unlock */
    /*
     * Lock() proves the d-half of the register file and nothing else -
     * thirteen descriptors take their string from A0 or A1. FindPort
     * takes its name in A1, touches no disk, and returns NULL for a
     * name nobody has, so it costs nothing to prove the other half.
     */
    (void)FindPort(path);
    if (isproc)
        me->pr_WindowPtr = oldwin;

    wasabi_snoop_on = 0;
    wasabi_snoop_entry = 0;
    g_snoop_self = self;

    if (lock)                        /* absurd, but do not leak it */
        UnLock(lock);

    for (i = g_snev_tail; i != g_snev_head; i = (i + 1) & (SN_EVMAX - 1)) {
        struct SnoopEv *ev = &g_snev[i];
        if (ev->fn == &snoop_desc_FindPort) {
            /* The A-register probe. Same rule as below: only OUR call
             * proves anything, anyone else's is just traffic. */
            if (strcmp(ev->s1, path) == 0)
                found_a = TRUE;
            continue;
        }
        if (ev->fn != &snoop_desc_Lock)
            continue;                /* another task's call, in the window */
        /*
         * And this is why the string is unique per attempt: snooping is
         * wide open during the window (g_snoop_self is NULL), so every
         * task on the machine lands in the ring. A Lock() that is not
         * ours is not a failure, it is somebody else working - skip it.
         * Failing here instead used to report "the trampoline and this
         * build disagree" whenever Workbench happened to touch a drawer
         * while snoop was starting.
         */
        if (strcmp(ev->s1, path) != 0)
            continue;
        if (ev->num != ACCESS_READ) {
            copystr(why, whysz, "the mode argument came back wrong");
            goto done;
        }
        /* The entry event is the new half of the trampoline: it proves
         * the register file survived the C call made before the
         * original ran. It carries no result, so it is checked for its
         * arguments and counted separately. */
        if (ev->entry) {
            found_entry = TRUE;
            continue;
        }
        if (ev->res != (LONG)lock) {
            copystr(why, whysz, "the result came back wrong");
            goto done;
        }
        found = TRUE;
    }
    if (found && !found_a) {
        copystr(why, whysz, "the address-register half came back wrong");
        found = FALSE;
        goto done;
    }
    if (found && entry && !found_entry) {
        copystr(why, whysz, "the entry-side patch captured nothing");
        found = FALSE;
        goto done;
    }
    if (found)
        copystr(why, whysz, "ok");
    else
        copystr(why, whysz, g_snev_lost ? "the ring overflowed"
                                              : "the patch captured nothing");
done:
    g_snev_head = g_snev_tail = g_snev_lost = 0;
    return found;
}

BOOL snoop_start(const char *pat, ULONG flags, char *why, LONG whysz)
{
    BOOL entry = (flags & SNOOPF_ENTRY) != 0;

    copystr(g_snoop_pat, sizeof(g_snoop_pat), pat);
    g_snev_head = g_snev_tail = g_snev_lost = 0;
    g_snoop_self = FindTask(NULL);
    snoop_install();

    /* Patches are live from here; nothing records until the flag is set.
     * The self-test runs in whichever mode the session asked for, so
     * entry logging is proven on this machine before any other task can
     * reach the new half of the trampoline. */
    if (!snoop_selftest(why, whysz, entry)) {
        snoop_uninstall();
        return FALSE;
    }

    wasabi_snoop_entry = entry ? 1 : 0;
    wasabi_snoop_on = 1;
    return TRUE;                     /* lines follow from snoop_next_line */
}

/*
 * One formatted line per call, filter applied. Returns 0 when the ring
 * is dry, so the daemon loops until it is - the caller owns the wire,
 * this owns the ring.
 */
LONG snoop_next_line(char *out, LONG max)
{
    while (g_snev_tail != g_snev_head) {
        struct SnoopEv *ev = &g_snev[g_snev_tail];
        BOOL want = (!g_snoop_pat[0] || pat_match(g_snoop_pat, ev->task));
        LONG n = 0;
        if (want && max >= SNOOP_LINE_MAX)
            n = snoop_format(ev, out);
        g_snev_tail = (g_snev_tail + 1) & (SN_EVMAX - 1);
        if (n)
            return n;                /* one line per call, filter applied */
    }
    return 0;
}

ULONG snoop_take_lost(void)
{
    ULONG n;
    Disable();                       /* the producer increments this from
                                      * any context; a plain read-then-
                                      * clear silently drops what lands
                                      * between the two */
    n = g_snev_lost;
    g_snev_lost = 0;
    Enable();
    return n;
}

UWORD snoop_busy(void)
{
    return wasabi_snoop_users;
}

void snoop_stop(void)
{
    wasabi_snoop_on = 0;
    wasabi_snoop_entry = 0;
    snoop_uninstall();               /* best effort; a stuck stub idles */
}

/* --- the guru report ------------------------------------------------ */

/*
 * Emu68 has no MMU, so Enforcer-style hit detection is off the table;
 * what this machine CAN say is which task died with which alert code,
 * in the moment before the guru takes the display. A SetFunction patch
 * on exec's Alert() (LVO -108 - the one call every crash path funnels
 * through) copies the code and the calling task's name into globals,
 * wakes the daemon, and then chains to the original so the alert still
 * shows on the Amiga itself.
 *
 * The report escapes when the scheduler still runs, and waits for the
 * next boot when it does not:
 *
 *  - Alert raised in task context (an application calling Alert(), or
 *    a trap handler running on the task's behalf): Signal() preempts
 *    straight into the daemon - it runs at priority 1 for exactly this
 *    reason - and the LOG line is on the wire before the original
 *    Alert() freezes the machine.
 *
 *  - Alert raised in supervisor mode or an interrupt: no task switch
 *    can happen before the freeze, so the line cannot leave now. But
 *    exec preserves the code in ExecBase->LastAlert across the warm
 *    reboot - that is how the post-reboot guru screen knows what to
 *    show - and this daemon's next life reports it to the first stream
 *    subscriber. A client that auto-reconnects closes the loop: the
 *    machine dies, comes back, and the guru code arrives by itself.
 *
 * The note function runs in the crashing context, which may own almost
 * nothing: no allocation, no I/O, no library call but Signal() (which
 * exec documents as interrupt-callable), bounded stack, absolute
 * addressing (no a4 setup, same as the other patches).
 */
static BOOL           g_alert_patched;
static volatile ULONG g_alert_code;
static volatile ULONG g_alert_taskaddr;
static char           g_alert_taskname[36];

#define GURU_SNAP 192                /* bytes of supervisor stack kept */
static UBYTE          g_alert_snap[GURU_SNAP];
static ULONG          g_alert_snapat;
static ULONG          g_alert_snaplen;
static volatile BOOL  g_alert_pending;

/*
 * T:lastguru is the durable half of the guru report: one line of plain
 * text, written whenever an alert is captured live (or found waiting
 * in ExecBase->LastAlert at startup) and replayed at the top of every
 * new stream subscription. T: is RAM-backed, so the note survives
 * daemon restarts and self-updates - and dies with the boot, which is
 * exactly when LastAlert takes over. `Type T:lastguru` on the Amiga
 * itself reads the same note.
 */
/* 320, not 192: the after-reboot line is 15+8+10+35(name)+5+8+1
 * +79(" at PC .. (vector N, fault ..)")+37+31(stamp)+1 = 230 worst
 * case. Sized here, once, with the arithmetic written down. */
static char g_guru_note[320];        /* empty = nothing to tell */
static BOOL g_guru_note_dirty;       /* file write still owed */

/*
 * The black box. Exec's LastAlert does not survive a reboot on Emu68
 * (proven by poking it and rebooting: it comes back FFFFFFFF), but
 * plain RAM content does - a marker written at the TOP of the fast
 * pool rode a reboot intact while one at the bottom was recycled by
 * the boot. So a DEADEND alert is stashed just under mh_Upper of the
 * highest fast MemHeader, and the daemon's next life finds it there.
 * A checksum keeps leftover garbage from faking a guru. Recoverable
 * alerts never touch it: for those the daemon is alive to write
 * T:lastguru properly.
 *
 * The region is AllocAbs()'d at startup and held for the daemon's
 * whole life, which is the difference between a black box and a bug.
 * Free memory on this machine is not blank: exec keeps its free list
 * IN it, as MemChunks. Writing 60 bytes into a chunk we do not own
 * corrupts that list, and the machine gurus with AN_MemCorrupt at
 * some unrelated FreeMem minutes later - which is precisely the kind
 * of delayed, unattributable failure this daemon exists to catch,
 * and an unforgivable thing for it to cause. Owning the region makes
 * the write legal; the content still survives the reboot, because a
 * reboot rebuilds the memory list without clearing the RAM under it.
 */
#define GURU_STASH_OFF 512           /* bytes below mh_Upper */

/*
 * The snapshot is the difference between "it died" and "it died HERE".
 * Exec runs trap handling in supervisor mode and pushes a longword
 * exception number below the CPU's own frame (RKRM Exec), so at Alert()
 * time the stack above us holds:
 *
 *     exception number   (longword, Exec's, CPU-independent)
 *     STATUS REGISTER    (word)   \
 *     PROGRAM COUNTER    (long)    | the CPU frame - SR and PC are at
 *     FORMAT | VECTOR    (word)   /  these offsets on every 68k made
 *     ...format-specific...
 *
 * We keep the raw bytes and work it out later, in daemon context, where
 * a mistake costs a wrong log line rather than a second guru.
 */
struct GuruStash {
    ULONG magic1, magic2;
    ULONG code, taskaddr;
    char  name[36];
    ULONG attn;                      /* AttnFlags, a hint for decoding */
    ULONG snapat;                    /* where the snapshot was taken */
    ULONG snaplen;
    UBYTE snap[GURU_SNAP];
    ULONG sum;                       /* MUST stay last: see guru_stash_sum */
};
#define GURU_MAGIC1 0x57534247UL     /* 'WSBG' */
#define GURU_MAGIC2 0x55525531UL     /* 'URU1' */

static struct GuruStash *g_stash;    /* ours, or NULL: no black box */
static ULONG g_stash_size;           /* what AllocAbs actually gave us */

static struct GuruStash *guru_stash_at(struct MemHeader *mh)
{
    return (struct GuruStash *)((UBYTE *)mh->mh_Upper - GURU_STASH_OFF);
}

/* Claim the region from the allocator, so writing to it is ours to do.
 * A failure is not fatal - it only means this boot has no black box,
 * and the live report and T:lastguru still work. */
/*
 * Where the black box lives: 8-aligned, GURU_STASH_OFF below the top of
 * the highest fast-RAM pool. Computing this takes nothing from anyone,
 * so a daemon that failed to claim the region can still read what the
 * last one left there.
 */
static struct GuruStash *guru_stash_top(void)
{
    struct MemHeader *mh, *best = NULL;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        if ((mh->mh_Attributes & MEMF_FAST) &&
            (!best || mh->mh_Upper > best->mh_Upper))
            best = mh;
    Permit();
    if (!best)
        return NULL;
    return (struct GuruStash *)(((ULONG)guru_stash_at(best)) & ~7UL);
}

void guru_claim(void)
{
    struct GuruStash *top = guru_stash_top();

    if (!top)
        return;
    /* An 8-aligned address and a size that is a multiple of 8, so exec
     * has nothing to round - and then free exactly the extent it handed
     * back rather than the one we asked for, because a FreeMem whose
     * bounds do not match its AllocMem is itself a corrupt memory
     * list. */
    g_stash = (struct GuruStash *)AllocAbs(GURU_STASH_OFF, (APTR)top);
    if (g_stash) {
        g_stash_size = ((ULONG)top + GURU_STASH_OFF) - (ULONG)g_stash;
        g_stash_size = (g_stash_size + 7) & ~7UL;
    }
}

/*
 * Give the black box back - but only if nothing can still write to it.
 *
 * If the Alert() patch could not be removed, our stub is still in the
 * vector and wasabi_alert_note() -> guru_stash_write() is still live.
 * Freeing the region then puts a MemChunk header where the next guru
 * will scribble its report, and the machine dies later with
 * AN_MemCorrupt at an unrelated FreeMem - which is precisely the bug
 * that owning this region was introduced to stop. A process that stays
 * resident keeps its memory.
 *
 * g_stash is cleared BEFORE the free, not after: an alert landing in
 * between would otherwise write into a chunk exec had just taken back.
 */
void guru_release(void)
{
    struct GuruStash *st = g_stash;
    ULONG size = g_stash_size;

    if (g_alert_patched)                 /* the hook outlived its removal */
        return;
    g_stash = NULL;
    g_stash_size = 0;
    if (st && size)
        FreeMem(st, size);
}

static ULONG guru_stash_sum(const struct GuruStash *st)
{
    const ULONG *w = (const ULONG *)st;
    ULONG sum = 0;
    int i, n = (sizeof(struct GuruStash) - sizeof(ULONG)) / sizeof(ULONG);
    for (i = 0; i < n; i++)
        sum += w[i];
    return sum;
}

/* Crashing-context rules apply: reads and plain stores only - and the
 * region was claimed at startup, so no list walking is needed here. */
static void guru_stash_write(void)
{
    struct GuruStash *st = g_stash;
    int i;
    if (!st)
        return;
    st->magic1 = GURU_MAGIC1;
    st->magic2 = GURU_MAGIC2;
    st->code = g_alert_code;
    st->taskaddr = g_alert_taskaddr;
    for (i = 0; i < (int)sizeof(st->name); i++)
        st->name[i] = g_alert_taskname[i];
    st->attn = (ULONG)SysBase->AttnFlags;
    st->snapat = g_alert_snapat;
    st->snaplen = g_alert_snaplen;
    for (i = 0; i < GURU_SNAP; i++)
        st->snap[i] = g_alert_snap[i];
    st->sum = guru_stash_sum(st);
}

/* Non-static: the asm stub below reaches both by symbol. */
APTR wasabi_alert_orig;
void wasabi_alert_note(ULONG code);
extern void wasabi_alert_patch(void);

/*
 * Entry: D7 = alert code (Alert's argument register, unusually), A6 =
 * ExecBase. Save the scratch registers around the C call - gcc
 * preserves d2-d7/a2-a6 itself, so D7 and A6 arrive at the original
 * untouched - then chain by pushing the original and rts'ing into it,
 * which spends no register at all.
 */
asm(
    "    .text                          \n"
    "    .even                          \n"
    "    .globl _wasabi_alert_patch     \n"
    "_wasabi_alert_patch:               \n"
    "    movem.l %d0-%d1/%a0-%a1,-(%sp) \n"
    "    move.l  %d7,-(%sp)             \n"
    "    jsr     _wasabi_alert_note     \n"
    "    addq.l  #4,%sp                 \n"
    "    movem.l (%sp)+,%d0-%d1/%a0-%a1 \n"
    "    move.l  _wasabi_alert_orig,-(%sp) \n"
    "    rts                            \n"
);

void wasabi_alert_note(ULONG code)
{
    struct Task *t = SysBase->ThisTask;
    const char *nm;
    LONG n = 0;
    UBYTE probe;                     /* its address is the stack pointer */

    /*
     * A second alert before the first was reported: keep the first, it
     * is the one that started the trouble. EXCEPT when the newcomer is
     * a deadend and the pending one is not - the deadend is about to
     * take the machine down, and it is the only one whose report has
     * to survive a reboot. Letting a stale recoverable block it loses
     * exactly the crash worth having.
     */
    if (g_alert_pending &&
        !((code & 0x80000000UL) && !(g_alert_code & 0x80000000UL)))
        return;
    g_alert_code = code;
    g_alert_taskaddr = (ULONG)t;
    /* Name the culprit the way snoop does: the CLI command being run
     * if there is one - 'Amelinium_040', not 'Background CLI' - else
     * the task name. Pointer reads only; this context may belong to a
     * machine already coming apart. */
    if (t && t->tc_Node.ln_Type == NT_PROCESS) {
        struct CommandLineInterface *cli = (struct CommandLineInterface *)
            BADDR(((struct Process *)t)->pr_CLI);
        if (cli) {
            UBYTE *b = (UBYTE *)BADDR(cli->cli_CommandName);
            if (b && b[0]) {                 /* a BSTR: length, then bytes */
                n = b[0];
                if (n > (LONG)sizeof(g_alert_taskname) - 1)
                    n = sizeof(g_alert_taskname) - 1;
                memcpy(g_alert_taskname, b + 1, n);
            }
        }
    }
    if (!n) {
        nm = (t && t->tc_Node.ln_Name) ? t->tc_Node.ln_Name : "(no name)";
        for (; n < (LONG)sizeof(g_alert_taskname) - 1 && nm[n]; n++)
            g_alert_taskname[n] = nm[n];
    }
    g_alert_taskname[n] = '\0';

    /*
     * The supervisor stack above us, verbatim.
     *
     * Being ON that stack is the test for "this is a trap, not a
     * program calling Alert() by hand": Exec does trap handling in
     * supervisor mode, so a CPU exception arrives here with SP inside
     * SysStkLower..SysStkUpper and the frame at higher addresses,
     * pushed before any of the calls that led here. A user-mode
     * Alert() lands on the task's own stack, fails the bounds test,
     * and simply records nothing rather than capturing noise.
     *
     * Copy only. Every interpretation happens later, in daemon
     * context - a misread here would be a second crash inside the
     * first one.
     */
    g_alert_snaplen = 0;
    {
        /*
         * Rounded DOWN to an even address, and that is not cosmetic.
         * Exec pushes its exception number at an even address - the 68k
         * keeps the stack even - and guru_find_frame() scans the
         * snapshot at even offsets only. So if this origin is odd,
         * every field of the frame lands at an odd offset inside the
         * copy and the decoder cannot find it, ever. `probe` is one
         * byte; the compiler puts it wherever it likes.
         *
         * Measured under FS-UAE (68030, JIT off) with tests/pcprobe.c:
         * the origin came back 0x40002313, the decoder found nothing,
         * and rounding it down recovered the exact PC. That failure is
         * silent - it looks identical to "this alert had no trap
         * frame", which is the answer on Emu68 anyway, so it could have
         * sat here indefinitely.
         */
        UBYTE *from = (UBYTE *)((ULONG)&probe & ~1UL);
        UBYTE *low  = (UBYTE *)SysBase->SysStkLower;
        UBYTE *top  = (UBYTE *)SysBase->SysStkUpper;
        if (from >= low && from < top) {
            LONG want = (LONG)(top - from);
            LONG i;
            if (want > GURU_SNAP)
                want = GURU_SNAP;
            for (i = 0; i < want; i++)
                g_alert_snap[i] = from[i];
            g_alert_snapat  = (ULONG)from;
            g_alert_snaplen = want;
        }
    }

    /* A deadend is going to reboot the machine before the daemon can
     * write any file - stash the report in the black box now, in this
     * context, while stores still land. */
    if (code & 0x80000000UL)
        guru_stash_write();
    g_alert_pending = TRUE;
    if (g_daemon_task && t != g_daemon_task)
        Signal(g_daemon_task, SIGBREAKF_CTRL_F);
}

void guru_arm(void)
{
    if (g_alert_patched)
        return;
    Disable();
    wasabi_alert_orig = SetFunction((struct Library *)SysBase, -108,
                                    (ULONG (*)())wasabi_alert_patch);
    Enable();
    g_alert_patched = TRUE;
}

/* Same removal rule as the RawPutChar patch: only restore the vector if
 * it still points at us, or we would unlink whoever chained on top. */
static BOOL alert_remove(void)
{
    APTR res;
    BOOL removed;
    if (!g_alert_patched)
        return TRUE;
    Disable();
    res = SetFunction((struct Library *)SysBase, -108,
                      (ULONG (*)())wasabi_alert_orig);
    if (res == (APTR)wasabi_alert_patch) {
        removed = TRUE;
    } else {
        SetFunction((struct Library *)SysBase, -108, (ULONG (*)())res);
        removed = FALSE;
    }
    Enable();
    if (removed)
        g_alert_patched = FALSE;
    return removed;
}

/*
 * Find the CPU exception frame in a snapshot of the supervisor stack.
 *
 * The anchor is Exec's, not the CPU's: a longword exception number sits
 * immediately below the frame (RKRM Exec), so a candidate is confirmed
 * when the vector offset the CPU wrote agrees with it - vector offset ==
 * exception number * 4. Two independent fields agreeing is what tells a
 * real frame from stack contents that merely look like one.
 *
 * SR (+$00) and PC (+$02) are at the same offsets on every 68k from the
 * 68000 up, so the PC needs no per-CPU knowledge at all. The fault
 * address does: it is at frame+$08 for the six-word ($2), eight-word
 * ($4, the 68060's access error) and 30-word ($7, the 68040's) frames.
 * The 68020/030 bus-fault frames ($A/$B) put it elsewhere and are not
 * decoded here.
 *
 * Byte-at-a-time reads: the PC sits at an even but not longword-aligned
 * offset, and a 68000 would fault on a misaligned longword access.
 * Daemon context - nothing here runs in the crashing task.
 */
static ULONG guru_be32(const UBYTE *b)
{
    return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) |
           ((ULONG)b[2] << 8) | (ULONG)b[3];
}

static UWORD guru_be16(const UBYTE *b)
{
    return (UWORD)(((UWORD)b[0] << 8) | (UWORD)b[1]);
}

static BOOL guru_find_frame(const UBYTE *snap, LONG len, ULONG *pc,
                            ULONG *fault, UWORD *vec, UWORD *fmt)
{
    LONG i;

    for (i = 0; i + 12 <= len; i += 2) {
        ULONG exc = guru_be32(snap + i);
        const UBYTE *f = snap + i + 4;
        UWORD fv;

        if (exc < 2 || exc > 63)     /* a plausible vector number */
            continue;
        fv = guru_be16(f + 6);
        if ((ULONG)(fv & 0x0FFF) != exc * 4)
            continue;                /* Exec and the CPU disagree */

        *vec = (UWORD)exc;
        *fmt = (UWORD)((fv >> 12) & 0x0F);
        *pc = guru_be32(f + 2);
        *fault = 0;
        if ((*fmt == 2 || *fmt == 4 || *fmt == 7) && i + 4 + 12 <= len)
            *fault = guru_be32(f + 8);
        return TRUE;
    }
    return FALSE;
}

/* " at PC 0x... (vector N, fault 0x...)", or "" when nothing was found.
 * Kept short: this rides the one line a stream greets you with. */
static void guru_where(char *out, LONG max, const UBYTE *snap, LONG len)
{
    ULONG pc = 0, fault = 0;
    UWORD vec = 0, fmt = 0;

    out[0] = '\0';
    if (len <= 0 || max < 64)
        return;
    if (!guru_find_frame(snap, len, &pc, &fault, &vec, &fmt))
        return;
    if (fault)
        sprintf(out, " at PC 0x%08lx (vector %ld, fault 0x%08lx)",
                (unsigned long)pc, (long)vec, (unsigned long)fault);
    else
        sprintf(out, " at PC 0x%08lx (vector %ld)",
                (unsigned long)pc, (long)vec);
}

static void guru_stamp(char *out)    /* "13-Aug-26 13:36:41", or "" */
{
    struct DateTime dt;
    char date[LEN_DATSTRING], time[LEN_DATSTRING];
    DateStamp(&dt.dat_Stamp);
    dt.dat_Format  = FORMAT_DOS;
    dt.dat_Flags   = 0;
    dt.dat_StrDay  = NULL;
    dt.dat_StrDate = date;
    dt.dat_StrTime = time;
    out[0] = '\0';
    if (DateToStr(&dt))
        sprintf(out, "%s %s", date, time);
}

/* Try now, and again on a later pump turn if T: was not ready - the
 * note must not be lost to writing it too early in the boot. */
static void guru_file_flush(void)
{
    BPTR fh;
    char nl = '\n';
    if (!g_guru_note_dirty || !g_guru_note[0])
        return;
    fh = Open("T:lastguru", MODE_NEWFILE);
    if (!fh)
        return;
    Write(fh, g_guru_note, strlen(g_guru_note));
    Write(fh, &nl, 1);
    Close(fh);
    g_guru_note_dirty = FALSE;
}

/*
 * The note, replayed to a fresh stream subscriber. Read back from the
 * file rather than the global so a note left by this daemon's previous
 * life still greets the first attach after a self-update.
 */
/*
 * The previous life's guru, if any - two places to look.
 *
 * ExecBase->LastAlert first: on real hardware exec preserves it across
 * a warm reboot (that is how the boot-time guru screen knows what to
 * show). On Emu68 it comes back FFFFFFFF no matter what died - proven
 * by poking a value in and rebooting - so the check costs nothing
 * there and works where exec cooperates. Retire whatever is found, or
 * one old crash would be rediscovered by every future daemon start.
 *
 * Then the black box, which is what actually survives on Emu68;
 * checked second so its richer report (task name included) wins the
 * note file when both exist. guru_claim() must already have run.
 */
void guru_boot_check(void)
{
    ULONG a0 = SysBase->LastAlert[0];
    ULONG a1 = SysBase->LastAlert[1];
    char when[2 * LEN_DATSTRING + 2];

    if (a0 != 0 && a0 != 0xFFFFFFFFUL) {
        guru_stamp(when);
        sprintf(g_guru_note, "ALERT #%08lx (task 0x%08lx) - from "
                             "before the last reboot (seen %s)",
                (unsigned long)a0, (unsigned long)a1, when);
        g_guru_note_dirty = TRUE;
        guru_file_flush();               /* retried later if T: balks */
        Disable();
        SysBase->LastAlert[0] = 0xFFFFFFFFUL;
        SysBase->LastAlert[1] = 0xFFFFFFFFUL;
        Enable();
    }
    /*
     * Read the black box even if the claim failed: ownership is needed
     * to WRITE it, never to read it, and a report that survived a
     * reboot is far too expensive to drop because AllocAbs came back
     * NULL. Only the "seen it, forget it" store is gated on ownership,
     * for the same reason the write is.
     */
    {
        struct GuruStash *st = g_stash ? g_stash : guru_stash_top();
        if (st && st->magic1 == GURU_MAGIC1 && st->magic2 == GURU_MAGIC2 &&
            st->sum == guru_stash_sum(st)) {
            char where[80];
            LONG snaplen = (LONG)st->snaplen;
            st->name[sizeof(st->name) - 1] = '\0';
            if (snaplen < 0 || snaplen > GURU_SNAP)
                snaplen = 0;         /* a stash from another build */
            guru_where(where, sizeof(where), st->snap, snaplen);
            guru_stamp(when);
            sprintf(g_guru_note, "DEADEND ALERT #%08lx in task '%s' "
                                 "(0x%08lx)%s - from before the last "
                                 "reboot (seen %s)",
                    (unsigned long)st->code, st->name,
                    (unsigned long)st->taskaddr, where, when);
            g_guru_note_dirty = TRUE;
            guru_file_flush();
        }
        if (g_stash)
            st->magic1 = 0;              /* report a crash once */
    }
}

BOOL guru_read_note(char *out, LONG max)
{
    char text[320];
    LONG n = 0;
    BPTR fh = Open("T:lastguru", MODE_OLDFILE);
    if (fh) {
        n = Read(fh, text, sizeof(text) - 1);
        Close(fh);
    }
    if (n <= 0 || max < (LONG)sizeof(text) + 32)
        return FALSE;
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r'))
        n--;
    text[n] = '\0';
    sprintf(out, "[wasabi: last guru: %s]\n", text);
    return TRUE;
}

/*
 * The live half of the report: one visible line on any stream open at
 * the moment of the alert - and the durable note for everyone later.
 * Nobody subscribed is no longer a reason to hold the report; the
 * note file is what waits, not the flag.
 */
BOOL guru_take_live(char *out, LONG max)
{
    char when[2 * LEN_DATSTRING + 2];
    char where[80];
    const char *kind;

    if (!g_alert_pending || max < 200)
        return FALSE;
    kind = (g_alert_code & 0x80000000UL) ? "DEADEND" : "RECOVERABLE";
    guru_where(where, sizeof(where), g_alert_snap, (LONG)g_alert_snaplen);
    sprintf(out, "[wasabi: %s ALERT #%08lx in task '%s' (0x%08lx)%s]\n",
            kind, (unsigned long)g_alert_code, g_alert_taskname,
            (unsigned long)g_alert_taskaddr, where);
    guru_stamp(when);
    sprintf(g_guru_note, "%s ALERT #%08lx in task '%s' (0x%08lx)%s - %s",
            kind, (unsigned long)g_alert_code, g_alert_taskname,
            (unsigned long)g_alert_taskaddr, where, when);
    g_guru_note_dirty = TRUE;
    g_alert_pending = FALSE;
    return TRUE;
}

void guru_retry_note(void)
{
    guru_file_flush();
}


/* --- teardown ------------------------------------------------------- */

void guru_disarm(void)
{
    alert_remove();
}

/*
 * TRUE while any vector still points into this program. The daemon asks
 * before it lets the shell unload the segment - see the comment at the
 * top of this file, and the guru of 13 August 2026 that wrote it.
 */
BOOL patches_stuck(void)
{
    /*
     * snoop_busy() counts callers parked between a stub's use-count
     * bump and its rts. They are not "a patch we failed to remove",
     * but they are the same hazard: the patched set includes
     * SystemTagList and Execute, which run whole commands, so a task
     * can sit inside our stub for minutes. Unloading under it drops
     * it into freed memory on the way back out.
     */
    return (BOOL)(g_dbg_patched || g_alert_patched || snoop_stuck() ||
                  snoop_busy() != 0);
}

/*
 * The last act of teardown, after the daemon has waited for callers to
 * leave the stubs. Separated from snoop_uninstall() precisely so the
 * open count outlives the stragglers.
 */
void patches_closelibs(void)
{
    snoop_closelibs();
}

void patches_set_daemon_task(struct Task *t)
{
    g_daemon_task = t;
}
