/*
 * latewrite - sleep first, write second. The mirror of slowwrite: where
 * that one proves a growing file is tailed live, this one proves output
 * survives when the FIRST byte arrives late. The daemon's tail handle
 * opens moments after the runner starts; ram-handler makes a handle
 * opened on a still-empty file permanently blind to later appends, so
 * before the ExamineFH/reopen cure in pump_run() this program's entire
 * output vanished. Anything that slept before writing - or buffered
 * stdio and flushed at exit - failed the same way.
 *
 * Expected: both lines arrive. Broken daemon: nothing arrives.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

int main(void)
{
    Delay(25);                          /* half a second of silence */
    Write(Output(), "late line 1\n", 12);
    Delay(25);
    Write(Output(), "late line 2\n", 12);
    return 0;
}
