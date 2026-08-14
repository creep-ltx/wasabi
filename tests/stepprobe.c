/*
 * stepprobe - does this machine deliver a TRACE exception to the guest?
 *
 * The open question left by tests/trapprobe.c. That probe proved a
 * deliberate `TRAP #n` reaches the guest's tc_TrapCode on Emu68, four
 * runs out of four. It proved nothing about single-step, because the
 * trace facility raises a CPU *exception* (vector 9) rather than a
 * software trap, and Emu68 has already been measured swallowing CPU
 * faults - an illegal instruction, a privilege violation and a
 * divide-by-zero each took the machine down with nothing reaching the
 * guest. Trace therefore sits on the far side of the one line we know
 * exists, and nothing measured so far predicts it either way.
 *
 * That distinction decides whether single-step is buildable here, which
 * is the natural companion to TRAP-based breakpoints.
 *
 * Build:
 *   $(HOME)/opt/amiga/bin/m68k-amigaos-gcc -O2 -noixemul \
 *       tests/stepprobe.c -o tests/stepprobe
 *
 *   stepprobe          Report the CPU. Reads only, cannot hurt anything.
 *   stepprobe step     Run the experiment. See the warning.
 *
 * HOW IT WORKS, and why it is shaped this way.
 *
 * The T1 bit of the status register turns on tracing, and reaching the
 * SR needs supervisor mode. Rather than call Supervisor() - which would
 * start tracing somewhere inside exec's own return path, with the first
 * traced instruction outside our control - this rides the mechanism
 * trapprobe already proved:
 *
 *   1. install a tc_TrapCode handler and execute TRAP #1;
 *   2. the handler sets T1 in the *saved* SR before its RTE, so tracing
 *      begins on the instruction after the trap - in this file;
 *   3. that instruction should raise a trace exception;
 *   4. the handler clears T1 on any hit after the first, so tracing is
 *      on for exactly one instruction.
 *
 * A second TRAP #1 follows, and it does double duty. If trace worked it
 * is merely a third harmless hit. If trace did NOT work, T1 is still set
 * in this task's SR, and that second trap is what gets the handler back
 * on the CPU to clear it before we return to the shell. So the count
 * tells the story by itself:
 *
 *   3 hits, second one vector 9   -> trace is delivered here
 *   2 hits, second one the trap   -> trace never fired
 *
 * The handler records raw exception numbers rather than matching against
 * constants, because a probe that only reports what it expected to find
 * has measured its own assumptions. The numbers are printed either way.
 *
 * WHAT A NEGATIVE RESULT WOULD AND WOULD NOT MEAN. This measures one
 * route: trace -> exec -> this task's tc_TrapCode. A "no trace" answer
 * has two possible causes and does not distinguish them - Emu68 may
 * have swallowed the exception the way it swallows other CPU faults, or
 * exec may handle vector 9 itself and never route it to a task. Either
 * way single-step is not buildable on tc_TrapCode here, which is the
 * question being asked; but it would not license the broader claim that
 * the guest never sees a trace at all. Distinguishing the two needs a
 * look at the vector table, which is a bigger and much more invasive
 * experiment than this one.
 *
 * RESULT, 14 August 2026, A1200 + PiStorm32-lite/CM4 on Emu68, CPU
 * reported as 68040+ (AttnFlags 0x807F):
 *
 *   2 hit(s); exception numbers 33, 33, 0     - four runs out of four
 *
 * No trace. T1 was set in the saved SR, the RTE returned with it set,
 * and the next instruction ran untraced; the second hit is the second
 * TRAP. Machine unharmed every time, nothing on the debug stream.
 *
 * So single-step is NOT available on this hardware, while TRAP-based
 * breakpoints are (see trapprobe.c). A debugger built on this machine
 * would be breakpoint-only - which is worth knowing before the design
 * rather than after it.
 *
 * One thing measured here for the first time as a side effect: exec's
 * exception number for TRAP #1 really is 33, the CPU vector 32 + n.
 * trapprobe counted hits without ever checking the number, so that had
 * been assumed rather than seen. It reads back 33 on both hits.
 *
 * Read the negative-result caveat above before quoting this: it rules
 * out the tc_TrapCode route, not every possible route.
 *
 * WARNING. This is an experiment, not a tool, and it is a step beyond
 * trapprobe: it deliberately turns on a CPU facility from inside an
 * exception handler. Everything it relies on below the T1 bit itself was
 * measured on this machine on 14 August 2026 - exec pushing the
 * exception number as a longword, the handler removing it and returning
 * with RTE - but T1 is new. If it goes wrong the machine goes down.
 * Run it when you can reach the keyboard.
 *
 * Blast radius is bounded on purpose: tc_TrapCode is per-task, so a
 * mistake damages this process rather than the machine, and the T bit
 * lives in this task's own SR context and dies with the process.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

extern struct ExecBase *SysBase;

/*
 * Written by the handler below, read by main(). Plain globals reached by
 * absolute address, the same rule the daemon's patches follow: an
 * exception handler has no a4 to set up.
 */
volatile ULONG wasabi_hits;
volatile ULONG wasabi_exc1;          /* our TRAP */
volatile ULONG wasabi_exc2;          /* the trace, if there is one */
volatile ULONG wasabi_exc3;          /* the second TRAP */

/*
 * Entered with exec's exception number as a longword at (sp), the CPU
 * frame above it. Saving d0/d1 puts the number at 8(sp) and the saved SR
 * - the one RTE will restore, and therefore the one whose T bit decides
 * whether the NEXT instruction traces - at 12(sp).
 *
 * The CPU clears T when it takes the exception, so this handler runs
 * untraced and cannot recurse into itself.
 *
 * 0x3FFF clears T1 and T0 and touches nothing else: bit 13 is the
 * supervisor bit, and the low byte is the caller's condition codes.
 */
extern void wasabi_step_handler(void);
asm(
    "    .text                          \n"
    "    .even                          \n"
    "    .globl _wasabi_step_handler    \n"
    "_wasabi_step_handler:              \n"
    "    move.l  %d0,-(%sp)             \n"
    "    move.l  %d1,-(%sp)             \n"
    "    move.l  8(%sp),%d0             \n"   /* exec's exception number */
    "    move.l  _wasabi_hits,%d1       \n"
    "    addq.l  #1,%d1                 \n"
    "    move.l  %d1,_wasabi_hits       \n"
    "    cmpi.l  #1,%d1                 \n"
    "    bne.s   1f                     \n"
    "    move.l  %d0,_wasabi_exc1       \n"
    "    ori.w   #0x8000,12(%sp)        \n"   /* T1 on for the next insn */
    "    bra.s   9f                     \n"
    "1:  cmpi.l  #2,%d1                 \n"
    "    bne.s   2f                     \n"
    "    move.l  %d0,_wasabi_exc2       \n"
    "    bra.s   8f                     \n"
    "2:  cmpi.l  #3,%d1                 \n"
    "    bne.s   8f                     \n"
    "    move.l  %d0,_wasabi_exc3       \n"
    "8:  andi.w  #0x3FFF,12(%sp)        \n"   /* T off, on every later hit */
    "9:  move.l  (%sp)+,%d1             \n"
    "    move.l  (%sp)+,%d0             \n"
    "    addq.l  #4,%sp                 \n"   /* drop the exception number */
    "    rte                            \n"
);

/* noinline so nothing can be reordered around it and nothing can decide
 * the trap is dead code. The instruction after `trap #1` is this
 * function's own epilogue, which is exactly where tracing should bite. */
static void __attribute__((noinline)) fire_trap(void)
{
    asm volatile ("trap #1");
}

static const char *cpu_name(UWORD f)
{
    if (f & AFF_68040) return "68040+";
    if (f & AFF_68030) return "68030";
    if (f & AFF_68020) return "68020";
    if (f & AFF_68010) return "68010";
    return "68000";
}

/* Exec's number for TRAP #n is the CPU vector, 32 + n - which trapprobe
 * did not check and this one reports rather than assumes. */
#define VEC_TRAP1  33
#define VEC_TRACE  9

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    UWORD flags = SysBase->AttnFlags;
    APTR oldtrap;
    ULONG hits, e1, e2, e3;

    Printf("stepprobe: CPU %s (AttnFlags 0x%04lx)\n",
           (LONG)cpu_name(flags), (long)flags);

    if (argc < 2 || strcmp(argv[1], "step") != 0) {
        Printf("stepprobe: pass 'step' to run the trace test itself - "
               "read the warning in the source first\n");
        return RETURN_OK;
    }

    wasabi_hits = wasabi_exc1 = wasabi_exc2 = wasabi_exc3 = 0;
    oldtrap = me->tc_TrapCode;
    me->tc_TrapCode = (APTR)wasabi_step_handler;

    Printf("stepprobe: handler installed; TRAP #1 to switch tracing on\n");
    Flush(Output());              /* say it before we might not come back */

    fire_trap();                  /* hit 1: sets T1. Next insn should trace */

    Printf("stepprobe: back from the traced instruction\n");
    Flush(Output());

    fire_trap();                  /* clears T1 if the trace never fired */

    me->tc_TrapCode = oldtrap;

    hits = wasabi_hits;
    e1 = wasabi_exc1;
    e2 = wasabi_exc2;
    e3 = wasabi_exc3;
    Printf("stepprobe: %lu hit(s); exception numbers %lu, %lu, %lu\n",
           (unsigned long)hits, (unsigned long)e1, (unsigned long)e2,
           (unsigned long)e3);

    if (hits >= 3 && e2 == VEC_TRACE) {
        Printf("stepprobe: RESULT - the trace exception WAS delivered "
               "(vector %lu between the two traps). Single-step is a live "
               "route on this machine.\n", (unsigned long)e2);
    } else if (hits == 2 && e2 == e1) {
        Printf("stepprobe: RESULT - no trace. T1 was set and the next "
               "instruction ran untraced; the second hit is the second "
               "TRAP, not a trace. Single-step is NOT available here.\n");
    } else {
        Printf("stepprobe: RESULT - unexpected pattern, read the numbers "
               "above rather than trusting this line. Expected 3 hits with "
               "a %lu in the middle, or 2 hits both %lu.\n",
               (unsigned long)VEC_TRACE, (unsigned long)VEC_TRAP1);
    }
    return RETURN_OK;
}
