/*
 * slowwrite - write one line every half second to Output(), unbuffered.
 * A deliberately slow, still-running writer: if `wasabi run` shows these
 * lines appear one at a time over four seconds, the daemon tails a
 * growing file live. If they all arrive at once at the end, it doesn't.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int count = (argc > 1) ? atoi(argv[1]) : 8;
    int i;
    char line[64];
    for (i = 0; i < count; i++) {
        int n = sprintf(line, "slow line %d\n", i);
        Write(Output(), line, n);
        Delay(25);                       /* half a second */
    }
    return 0;
}
