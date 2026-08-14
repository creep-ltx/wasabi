/*
 * trapprobe - does this machine deliver a CPU exception to the guest OS?
 *
 * Everything measured so far about exceptions on Emu68 concerns
 * *involuntary* ones: an illegal instruction, a privilege violation and a
 * divide-by-zero each took the PiStorm32 down without the guest seeing
 * anything (see the guru report section of README.md). A `TRAP #n` is
 * different in kind - it is an instruction the program deliberately
 * executes - and nothing about the illegal-instruction result implies an
 * answer for it. That question is worth settling, because it decides
 * whether breakpoints, single-step and any application-crash reporting
 * are buildable on this hardware at all, or whether the whole direction
 * is dormant here the way the guru PC decoder already is.
 *
 * Build (Bebbo's cross-compiler, same as the daemon):
 *   $(HOME)/opt/amiga/bin/m68k-amigaos-gcc -O2 -noixemul \
 *       tests/trapprobe.c -o tests/trapprobe
 *
 * Two modes, because they carry very different risk:
 *
 *   trapprobe          Report the CPU and, on a 68010+, the VBR. Reads
 *                      only. Cannot hurt the machine.
 *
 *   trapprobe trap     Install a tc_TrapCode handler for THIS process,
 *                      execute TRAP #1, restore, and report whether the
 *                      handler ran. See the warning below.
 *
 * RESULT, 14 August 2026, A1200 + PiStorm32-lite/CM4 on Emu68, CPU
 * reported as 68040+ (AttnFlags 0x807F), VBR 0x00000000:
 *
 *   the handler ran, four runs out of four, machine unharmed.
 *
 * So Emu68 DOES deliver a deliberate trap to the guest, and tc_TrapCode
 * is a live route here - which the VBR reading of 0 had suggested it
 * would not be. Note precisely what that does and does not settle:
 * software traps pass through, while CPU *faults* still do not (an
 * illegal instruction, a privilege violation and a divide-by-zero each
 * took the machine down with nothing reaching the guest). Breakpoints
 * are therefore buildable on this hardware; catching an application
 * crash is still not. Single-step sits on the far side of that line -
 * the trace bit raises a fault, not a trap - and remains untested.
 *
 * WARNING for `trapprobe trap`. This is an experiment, not a tool. It
 * relies on exec's documented trap convention - the exception number
 * pushed as a longword below the CPU frame, the handler responsible for
 * removing it and returning with RTE - and if that convention does not
 * hold here, the RTE returns somewhere wrong and the machine goes down.
 * It is also possible the machine simply dies at the TRAP itself, which
 * is one of the answers this is asking for. Run it when you can reach
 * the keyboard, not on a machine you need.
 *
 * The markers either side of the TRAP are deliberate. A crash test that
 * has not been proven to reach its crash has measured nothing (the first
 * attempt at the illegal-instruction test used `4321 / zero`, which
 * Bebbo's gcc turns into a helper call that never traps, and reported a
 * confident, meaningless zero). If this machine dies, the last line on
 * the `wasabi run` stream says exactly how far it got.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

extern struct ExecBase *SysBase;

/*
 * Exec's Supervisor() enters the routine in supervisor mode and requires
 * it to return with RTE. A C function compiles to RTS and takes the
 * machine down - which is why this is raw asm and not a static inline.
 * The same rule governs the trap handler below.
 *
 * movec is 68010 and up. On a 68000 there is no VBR at all and the
 * vector table is fixed at address 0, so the caller must check AttnFlags
 * before this ever runs: reading the VBR to find out whether you can
 * read the VBR is its own illegal instruction.
 */
ULONG wasabi_vbr;

extern void wasabi_read_vbr(void);
asm(
    "    .text                       \n"
    "    .even                       \n"
    "    .globl _wasabi_read_vbr     \n"
    "_wasabi_read_vbr:               \n"
    /* movec %vbr,%d0 - emitted as raw words on purpose. The daemon and
     * its tools are built for 68000, so the assembler refuses the
     * mnemonic, and raising the whole build to -m68020 to get one
     * instruction would make the binary unrunnable on the machines
     * where the AttnFlags check exists to protect it. Verified with
     * objdump: 4e7a 0801 disassembles to `movec vbr,d0`. */
    "    .word   0x4E7A, 0x0801      \n"
    "    move.l  %d0,_wasabi_vbr     \n"
    "    rte                         \n"
);

/*
 * The handler, doing as close to nothing as a handler can: bump a
 * counter, drop the exception number exec pushed, RTE. No allocation, no
 * I/O, no library call - this runs in supervisor mode on the supervisor
 * stack, and the daemon's own patch rules are lenient by comparison.
 *
 * A TRAP's saved PC already points past the instruction, so unlike a
 * fault there is no PC to fix up before returning.
 */
volatile ULONG wasabi_trap_hits;

extern void wasabi_trap_handler(void);
asm(
    "    .text                          \n"
    "    .even                          \n"
    "    .globl _wasabi_trap_handler    \n"
    "_wasabi_trap_handler:              \n"
    "    addq.l  #1,_wasabi_trap_hits   \n"
    "    addq.l  #4,%sp                 \n"   /* exec's exception number */
    "    rte                            \n"
);

/* The deliberate exception, on its own so nothing can be inlined around
 * it and nothing can decide it is dead code. */
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

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    UWORD flags = SysBase->AttnFlags;
    APTR oldtrap;
    ULONG hits;

    Printf("trapprobe: CPU %s (AttnFlags 0x%04lx)\n",
           (LONG)cpu_name(flags), (long)flags);

    if (flags & AFF_68010) {
        Supervisor((ULONG (*)())wasabi_read_vbr);
        Printf("trapprobe: VBR = 0x%08lx\n", (long)wasabi_vbr);
    } else {
        Printf("trapprobe: 68000 - no VBR; the vector table is at 0\n");
    }

    if (argc < 2 || strcmp(argv[1], "trap") != 0) {
        Printf("trapprobe: pass 'trap' to run the exception test itself "
               "- read the warning in the source first\n");
        return RETURN_OK;
    }

    /*
     * Per-task, not the vector table. tc_TrapCode belongs to this
     * process alone, so a wrong guess damages this process rather than
     * every task on the machine - and it is the route worth testing,
     * because it is the one an application-crash report would use.
     */
    wasabi_trap_hits = 0;
    oldtrap = me->tc_TrapCode;
    me->tc_TrapCode = (APTR)wasabi_trap_handler;

    Printf("trapprobe: handler installed, about to execute TRAP #1\n");
    Flush(Output());              /* say it before we might not come back */

    fire_trap();

    Printf("trapprobe: returned from TRAP #1\n");
    me->tc_TrapCode = oldtrap;

    hits = wasabi_trap_hits;
    if (hits) {
        Printf("trapprobe: RESULT - handler ran (%lu hit(s)). This machine "
               "DOES deliver a deliberate trap to the guest, and "
               "tc_TrapCode is a live route here.\n", (unsigned long)hits);
    } else {
        Printf("trapprobe: RESULT - we came back but the handler never ran. "
               "Something below the guest swallowed the trap; tc_TrapCode "
               "is not a route on this machine.\n");
    }
    return RETURN_OK;
}
