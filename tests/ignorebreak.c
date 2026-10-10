/*
 * ignorebreak - a command that will not stop for Ctrl-C, like a hung
 * test program: it clears the break signals every second for N seconds
 * (default 30), then exits by itself with 0. `wasabi free` on it must
 * let go of the slot after three seconds, and the runner must still end
 * cleanly when this finally exits - even if the daemon restarted
 * meanwhile and left its old code in memory for that runner.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 30;
    int i;
    char line[64];
    for (i = 0; i < secs; i++) {
        SetSignal(0, SIGBREAKF_CTRL_C);  /* swallow any Ctrl-C */
        if (i % 5 == 0) {
            int n = sprintf(line, "ignoring Ctrl-C, %d s left\n", secs - i);
            Write(Output(), line, n);
        }
        Delay(50);
    }
    Write(Output(), "done\n", 5);
    return 0;
}
