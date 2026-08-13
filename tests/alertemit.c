/*
 * alertemit - raise a RECOVERABLE alert on purpose, for testing the
 * Wasabi guru report end to end. If a subscribed `wasabi debug` shows
 *
 *   [wasabi: RECOVERABLE ALERT #35000000 in task 'alertemit' (0x...)]
 *
 * the whole chain works on real hardware: the Alert() patch captured
 * the code in this task's context, Signal()ed the daemon awake, and
 * the daemon's priority won the race against the alert display.
 *
 * The alert itself still appears on the Amiga - a flashing recoverable
 * alert, dismissed with the left mouse button - because the patch
 * chains to the original. Deliberately NOT a deadend alert: that path
 * freezes the machine and is tested by ExecBase->LastAlert reporting
 * after the reboot, which any real guru exercises for free.
 *
 *   alertemit [hexcode]     default 35000000 (AN_BadFreeAddr, deadend
 *                           bit cleared - recognizable and harmless)
 *   alertemit deadend [hexcode]
 *                           raise a real DEADEND alert: the machine
 *                           freezes with a guru and reboots on the
 *                           click. Tests the black-box path - after
 *                           the reboot, T:lastguru must name this
 *                           task and code. Default 8035C0DE.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    ULONG code = 0x35000000UL;
    int deadend = argc > 1 && !strcmp(argv[1], "deadend");

    if (deadend)
        code = argc > 2 ? ((ULONG)strtoul(argv[2], NULL, 16)
                           | 0x80000000UL)
                        : 0x8035C0DEUL;
    else if (argc > 1)
        code = (ULONG)strtoul(argv[1], NULL, 16) & 0x7FFFFFFFUL;

    if (deadend)
        Printf("alertemit: raising DEADEND alert #%08lx - the machine "
               "WILL guru\nand reboot on the click. T:lastguru must name "
               "it afterwards.\n", (unsigned long)code);
    else
        Printf("alertemit: raising recoverable alert #%08lx - click the "
               "left\nmouse button on the Amiga to dismiss it\n",
               (unsigned long)code);
    Alert(code);
    Printf("alertemit: back - check the wasabi debug/snoop terminal\n");
    return 0;
}
