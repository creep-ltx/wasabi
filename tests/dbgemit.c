/*
 * dbgemit - emit serial debug output via RawPutChar, for testing the
 * Wasabi debug stream end to end. This is exactly the path Sashimi
 * intercepts, so if `wasabi debug` shows these lines, the whole chain
 * works on real hardware.
 *
 *   dbgemit [count]     default 5 lines
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * exec's RawPutChar (LVO -516) isn't in Bebbo's inline headers, so call
 * the vector directly: char in D0, ExecBase in A6. This is the exact
 * function Sashimi patches - and the one Wasabi's own debug patch will
 * eventually replace it with.
 */
static void raw_put(unsigned char c)
{
    register unsigned char d0v asm("d0") = c;
    register void *a6v asm("a6") = (void *)SysBase;
    asm volatile ("jsr -516(%%a6)"
                  : "+r"(d0v)
                  : "r"(a6v)
                  : "d1", "a0", "a1", "cc", "memory");
}

int main(int argc, char **argv)
{
    int count = (argc > 1) ? atoi(argv[1]) : 5;
    int i;
    char line[80];

    for (i = 0; i < count; i++) {
        int len = sprintf(line, "WASABI DEBUG TEST line %d of %d\n",
                          i + 1, count);
        int j;
        for (j = 0; j < len; j++)
            raw_put((unsigned char)line[j]);
        Delay(10);                       /* ~1/5s between lines */
    }
    Printf("dbgemit: sent %ld line(s) via RawPutChar\n", (long)count);
    return 0;
}
