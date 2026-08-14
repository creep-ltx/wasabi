/* Host test of the REAL decoder source, lifted verbatim from patches.c,
 * against frames built by hand from M68000PM Appendix B. */
#include <stdio.h>
#include <string.h>
typedef unsigned long  ULONG;
typedef unsigned short UWORD;
typedef unsigned char  UBYTE;
typedef long           LONG;
typedef int            BOOL;
#define TRUE 1
#define FALSE 0
#include "dec.inc"

static int fails;
static UBYTE buf[192];

static void put16(int o, unsigned v){ buf[o]=v>>8; buf[o+1]=v; }
static void put32(int o, unsigned long v){
    buf[o]=v>>24; buf[o+1]=v>>16; buf[o+2]=v>>8; buf[o+3]=v; }

/* Exec pushes the exception number, then the CPU frame follows. */
static void frame(int at, unsigned exc, unsigned sr, unsigned long pc,
                  unsigned fmt, unsigned long ea)
{
    put32(at, exc);
    put16(at+4, sr);
    put32(at+6, pc);
    put16(at+10, (fmt<<12) | (exc*4));
    if (ea) put32(at+12, ea);
}

static void want(const char *what, int ok, unsigned long got,
                 unsigned long exp)
{
    if (ok && got == exp) printf("  ok    %-34s 0x%08lx\n", what, got);
    else { printf("  FAIL  %-34s got 0x%08lx want 0x%08lx (found=%d)\n",
                  what, got, exp, ok); fails++; }
}

int main(void)
{
    ULONG pc, fault; UWORD vec, fmt; BOOL ok;

    /* Format $0 - illegal instruction, vector 4. PC points AT it. */
    memset(buf,0,sizeof buf); frame(0, 4, 0x2700, 0x00123456, 0, 0);
    ok = guru_find_frame(buf, sizeof buf, &pc, &fault, &vec, &fmt);
    want("fmt $0 illegal, PC", ok, pc, 0x00123456);
    want("fmt $0 illegal, vector", ok, vec, 4);

    /* Format $2 - divide by zero, vector 5, EA at frame+8 */
    memset(buf,0,sizeof buf); frame(0, 5, 0x2700, 0x00200010, 2, 0xDEAD0000);
    ok = guru_find_frame(buf, sizeof buf, &pc, &fault, &vec, &fmt);
    want("fmt $2 divzero, PC", ok, pc, 0x00200010);
    want("fmt $2 divzero, fault addr", ok, fault, 0xDEAD0000);

    /* Format $7 - 68040 access error, vector 2 */
    memset(buf,0,sizeof buf); frame(0, 2, 0x2704, 0x07654321, 7, 0x00BEEF00);
    ok = guru_find_frame(buf, sizeof buf, &pc, &fault, &vec, &fmt);
    want("fmt $7 040 access err, PC", ok, pc, 0x07654321);
    want("fmt $7 040 access err, fault", ok, fault, 0x00BEEF00);

    /* Format $4 - 68060 access error, vector 2 */
    memset(buf,0,sizeof buf); frame(0, 2, 0x2704, 0x01020304, 4, 0x0A0B0C0D);
    ok = guru_find_frame(buf, sizeof buf, &pc, &fault, &vec, &fmt);
    want("fmt $4 060 access err, fault", ok, fault, 0x0A0B0C0D);

    /* Buried 40 bytes into the snapshot, with junk before it */
    memset(buf,0xA5,sizeof buf); frame(40, 8, 0x2700, 0x00998877, 0, 0);
    ok = guru_find_frame(buf, sizeof buf, &pc, &fault, &vec, &fmt);
    want("privilege violation, buried", ok, pc, 0x00998877);
    want("privilege violation, vector", ok, vec, 8);

    /* Exec's number and the CPU's vector DISAGREE -> must reject */
    memset(buf,0,sizeof buf); frame(0, 4, 0x2700, 0x00123456, 0, 0);
    put16(10, (0<<12) | (9*4));            /* claim vector 9 instead */
    ok = guru_find_frame(buf, sizeof buf, &pc, &fault, &vec, &fmt);
    if (!ok) printf("  ok    %-34s rejected\n", "mismatched vector");
    else { printf("  FAIL  mismatched vector accepted\n"); fails++; }

    /* Pure garbage -> must find nothing */
    memset(buf,0x5A,sizeof buf);
    ok = guru_find_frame(buf, sizeof buf, &pc, &fault, &vec, &fmt);
    if (!ok) printf("  ok    %-34s rejected\n", "garbage");
    else { printf("  FAIL  garbage accepted as PC 0x%08lx\n", pc); fails++; }

    /* A frame at an ODD offset must NOT be found - the scan steps by 2
     * from 0 and Exec pushes at even addresses, so odd is by definition
     * not a real frame. This documents the invariant the CAPTURE side
     * has to keep: patches.c rounds its copy origin down to even, and
     * without that a real frame lands at odd offsets and disappears.
     * Measured for real under FS-UAE before it was fixed - see
     * tests/pcprobe.c. */
    memset(buf,0,sizeof buf); frame(41, 4, 0x2700, 0x00123456, 0, 0);
    ok = guru_find_frame(buf, sizeof buf, &pc, &fault, &vec, &fmt);
    if (!ok) printf("  ok    %-34s rejected\n", "frame at an odd offset");
    else { printf("  FAIL  odd-offset frame accepted as PC 0x%08lx\n", pc);
           fails++; }

    printf("\n%s\n", fails ? "FAILURES" : "all decoder cases pass");
    return fails ? 1 : 0;
}
