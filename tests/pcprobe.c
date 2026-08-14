/*
 * pcprobe - can the guru report's decoder find a REAL exception frame?
 *
 * The guru PC capture has never produced a PC anywhere. It is proven
 * only by tests/dectest.c, which runs the decoder against frames built
 * by hand from M68000PM Appendix B - a good test of the arithmetic, and
 * no test at all of the assumption underneath it: that at exception
 * time the supervisor stack really does hold Exec's exception number as
 * a longword immediately below a CPU frame the decoder can recognise.
 *
 * That assumption has never met a frame the CPU actually pushed.
 *
 * This closes the gap without needing a crash. A `TRAP #n` produces a
 * genuine CPU exception frame - real SR, real PC, real format/vector
 * word - and unlike a fault it is survivable, so the machine is still
 * there afterwards to report what was found. The probe:
 *
 *   1. installs a tc_TrapCode handler that snapshots the supervisor
 *      stack exactly the way patches.c's Alert() hook does;
 *   2. executes TRAP #1 at an address it records;
 *   3. runs the REAL decoder - guru_find_frame(), extracted from
 *      patches.c at build time, not a copy - over the snapshot;
 *   4. checks the PC it recovers against the address it knows is right.
 *
 * Step 4 is the point. A decoder that finds *a* frame proves little; a
 * decoder that recovers the exact address of the instruction after the
 * trap has been checked against ground truth.
 *
 * Build (the decoder is lifted from patches.c so this cannot drift):
 *   awk '/^static ULONG guru_be32/,/^}/'        patches.c >  dec.inc
 *   awk '/^static UWORD guru_be16/,/^}/'        patches.c >> dec.inc
 *   awk '/^static BOOL guru_find_frame/,/^\}$/' patches.c >> dec.inc
 *   $(HOME)/opt/amiga/bin/m68k-amigaos-gcc -O2 -noixemul -I. \
 *       tests/pcprobe.c -o tests/pcprobe
 *
 * Safe by construction: TRAP is survivable and already measured on both
 * targets, tc_TrapCode is per-task, and nothing here writes a vector
 * table. It is not the crash test - it cannot be, because on Emu68 a
 * crash never arrives - it is the decoder test.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>

extern struct ExecBase *SysBase;

#include "dec.inc"                   /* guru_be32, guru_be16, guru_find_frame */

/* Same size as the daemon's black box keeps, for the same reason: it is
 * as much of the supervisor stack as is worth carrying. */
#define PC_SNAP 192

UBYTE          wasabi_pc_snap[PC_SNAP];
volatile ULONG wasabi_pc_snaplen;
volatile ULONG wasabi_pc_hits;
volatile ULONG wasabi_expected_pc;
volatile ULONG wasabi_pc_origin;   /* raw &probe, before any alignment */

/*
 * Called from the handler below, in supervisor mode. This is a
 * deliberate copy of what wasabi_alert_note() does in patches.c: take
 * the address of a local as "where we are on the stack", check it is
 * inside SysStkLower..SysStkUpper, and copy upward - bytes only, no
 * interpretation. Everything above this frame is the saved registers,
 * Exec's exception number and the CPU's own frame.
 */
void wasabi_pc_note(void)
{
    UBYTE  probe;
    UBYTE *from = (UBYTE *)&probe;
    UBYTE *low  = (UBYTE *)SysBase->SysStkLower;
    UBYTE *top  = (UBYTE *)SysBase->SysStkUpper;
    LONG   want, i;

    wasabi_pc_hits++;
    wasabi_pc_snaplen = 0;
    wasabi_pc_origin = (ULONG)from;
    if (from < low || from >= top)
        return;                      /* not the supervisor stack */
    /* Round the origin DOWN to an even address. Exec pushes its
     * exception number at an even address - the 68k keeps the stack
     * even - so an odd origin puts every field of the frame at an odd
     * offset inside the snapshot, and guru_find_frame scans i += 2 from
     * 0. It would never look there. `probe` is a single byte and the
     * compiler may place it anywhere. */
    from = (UBYTE *)((ULONG)from & ~1UL);
    want = (LONG)(top - from);
    if (want > PC_SNAP)
        want = PC_SNAP;
    for (i = 0; i < want; i++)
        wasabi_pc_snap[i] = from[i];
    wasabi_pc_snaplen = (ULONG)want;
}

extern void wasabi_pc_handler(void);
asm(
    "    .text                          \n"
    "    .even                          \n"
    "    .globl _wasabi_pc_handler      \n"
    "_wasabi_pc_handler:                \n"
    "    movem.l %d0-%d1/%a0-%a1,-(%sp) \n"
    "    jsr     _wasabi_pc_note        \n"
    "    movem.l (%sp)+,%d0-%d1/%a0-%a1 \n"
    "    addq.l  #4,%sp                 \n"   /* Exec's exception number */
    "    rte                            \n"
);

/*
 * The trap, and the ground truth to check the decoder against. Label 1
 * is the instruction after the TRAP, which is exactly what the CPU
 * stores as the saved PC for a trap exception - so its address is the
 * right answer, known independently of anything the decoder does.
 */
static void __attribute__((noinline)) fire_trap(void)
{
    asm volatile (
        "    trap    #1                 \n"
        "1:  lea     1b,%%a0            \n"
        "    move.l  %%a0,%0            \n"
        : "=m" (wasabi_expected_pc) :: "a0");
}

static const char *cpu_name(UWORD f)
{
    if (f & AFF_68040) return "68040+";
    if (f & AFF_68030) return "68030";
    if (f & AFF_68020) return "68020";
    if (f & AFF_68010) return "68010";
    return "68000";
}

int main(void)
{
    struct Task *me = FindTask(NULL);
    APTR  oldtrap;
    ULONG pc = 0, fault = 0;
    UWORD vec = 0, fmt = 0;
    LONG  len;

    Printf("pcprobe: CPU %s (AttnFlags 0x%04lx)\n",
           (LONG)cpu_name(SysBase->AttnFlags),
           (long)SysBase->AttnFlags);

    oldtrap = me->tc_TrapCode;
    me->tc_TrapCode = (APTR)wasabi_pc_handler;
    fire_trap();
    me->tc_TrapCode = oldtrap;

    len = (LONG)wasabi_pc_snaplen;
    Printf("pcprobe: %lu trap(s), %ld byte(s) captured from 0x%08lx "
           "(%s)\n", (unsigned long)wasabi_pc_hits, (long)len,
           (unsigned long)wasabi_pc_origin,
           (LONG)((wasabi_pc_origin & 1) ? "ODD - realigned before copy"
                                         : "even"));

    if (!wasabi_pc_hits) {
        Printf("pcprobe: RESULT - no trap reached the handler; nothing to "
               "decode. This machine does not deliver traps to the guest.\n");
        return RETURN_WARN;
    }
    if (len <= 0) {
        Printf("pcprobe: RESULT - the trap arrived but the stack pointer "
               "was outside SysStkLower..SysStkUpper, so the hook captured "
               "nothing. The guru report would record no PC here.\n");
        return RETURN_WARN;
    }

    if (!guru_find_frame(wasabi_pc_snap, len, &pc, &fault, &vec, &fmt)) {
        Printf("pcprobe: RESULT - FRAME NOT FOUND. The decoder could not "
               "match Exec's exception number against the CPU's vector "
               "offset in a real frame. Expected PC was 0x%08lx.\n",
               (unsigned long)wasabi_expected_pc);
        return RETURN_WARN;
    }

    Printf("pcprobe: decoded PC 0x%08lx, vector %ld, format $%lx, "
           "fault 0x%08lx\n", (unsigned long)pc, (long)vec,
           (unsigned long)fmt, (unsigned long)fault);
    Printf("pcprobe: expected PC 0x%08lx (the instruction after the "
           "TRAP)\n", (unsigned long)wasabi_expected_pc);

    if (pc == wasabi_expected_pc && vec == 33) {
        Printf("pcprobe: RESULT - CORRECT. The decoder found a real CPU "
               "frame and recovered the exact PC. The guru report's "
               "where-it-crashed half works against genuine hardware "
               "frames, not just hand-built ones.\n");
        return RETURN_OK;
    }
    Printf("pcprobe: RESULT - MISMATCH. A frame was found but it is not "
           "the right one (expected vector 33). Read the numbers above "
           "rather than trusting this line.\n");
    return RETURN_WARN;
}
