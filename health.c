/*
 * health.c - the machine's vital signs, as "key value" lines.
 *
 * Three sources, each used only where it exists, so one HEALTH command
 * answers on the A1200, in FS-UAE and on a plain 68000 alike - a field
 * the machine cannot provide is simply absent:
 *
 *   exec            free and largest chip/fast memory: everywhere
 *   Emu68           version, counters (uptime, 68k and ARM instructions,
 *                   JIT cache): only when the device tree has /emu68
 *   the Pi firmware temperature, voltages, clocks, the throttled flags:
 *                   only through mailbox.resource
 *
 * Emu68's counters are custom MOVEC registers, and MOVEC with those
 * numbers is an illegal-instruction guru on a real 68k and in FS-UAE.
 * Detection is therefore devicetree.resource AND an /emu68 node, the
 * same gate Emu68CP and EmuControl use, and the MOVECs are hand-
 * encoded words so this file builds for - and the daemon still runs
 * on - a plain 68000.
 *
 * The firmware is reached only through mailbox.resource (in Emu68's
 * own ROM since 1.1): it owns the mailbox under a semaphore, so this
 * can never collide with VideoCore.card or Emu68 itself mid-
 * transaction. A raw-MMIO fallback is deliberately not here. Header
 * files in include/ are the sfdc output from Emu68-tools, copied from
 * Emu68CP.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#define __NOLIBBASE__
#define DEVICETREE_BASE_NAME g_dt
#define MAILBOX_BASE_NAME g_mb
#include <proto/devicetree.h>
#include <proto/mailbox.h>
#undef __NOLIBBASE__

#include "health.h"

extern struct ExecBase *SysBase;

static APTR g_dt;                       /* resources: never closed */
static APTR g_mb;
static BOOL g_probed, g_emu68;

static void probe(void)
{
    APTR key;

    if (g_probed)
        return;
    g_probed = TRUE;
    g_dt = OpenResource("devicetree.resource");
    if (g_dt && (key = DT_OpenKey("/emu68")) != NULL) {
        DT_CloseKey(key);
        g_emu68 = TRUE;
    }
    g_mb = OpenResource("mailbox.resource");
}

/* A string property, bounded and always terminated - DT strings are
 * not promised to be (bootargs is not). */
static BOOL dt_str(const char *path, const char *prop, char *out, LONG n)
{
    APTR key, p;
    const char *v;
    ULONG len;

    out[0] = '\0';
    if (!g_dt || !(key = DT_OpenKey((CONST_STRPTR)path)))
        return FALSE;
    p = DT_FindProperty(key, (CONST_STRPTR)prop);
    v = p ? (const char *)DT_GetPropValue(p) : NULL;
    if (v) {
        len = DT_GetPropLen(p);
        if (len > (ULONG)n - 1)
            len = n - 1;
        memcpy(out, v, len);
        out[len] = '\0';
        for (len = 0; out[len]; len++)  /* one line each, always */
            if (out[len] == '\n' || out[len] == '\r')
                out[len] = ' ';
    }
    DT_CloseKey(key);
    return v != NULL;
}

/* --- Emu68's MOVEC counters ---------------------------------------- */

/*
 * movec Rc,d0 is 0x4e7a then 0x0000|Rc. Encoded as words so the
 * assembler never needs -m68020: this code is only ever reached after
 * the /emu68 check, and the rest of the daemon stays 68000 code.
 */
#define MOVEC_D0(rc, out) \
    asm volatile(".short 0x4e7a, %c1\n\tmove.l %%d0,%0" \
                 : "=g"(out) : "i"(rc) : "d0")

struct E68Counters {
    ULONG frq, cnt_hi, cnt_lo, insn_hi, insn_lo, arm_hi, arm_lo;
    ULONG jit_size, jit_free, jit_count, jit_miss;
};

static void read_counters(struct E68Counters *c)
{
    APTR ssp = SuperState();
    ULONG hi2;

    MOVEC_D0(0xe0, c->frq);
    MOVEC_D0(0xe7, c->jit_size);
    MOVEC_D0(0xe8, c->jit_free);
    MOVEC_D0(0xe9, c->jit_count);
    MOVEC_D0(0xec, c->jit_miss);
    /* 64-bit counters: hi, lo, hi again - retry if it rolled over */
    do {
        MOVEC_D0(0xe2, c->cnt_hi);
        MOVEC_D0(0xe1, c->cnt_lo);
        MOVEC_D0(0xe2, hi2);
    } while (hi2 != c->cnt_hi);
    do {
        MOVEC_D0(0xe4, c->insn_hi);
        MOVEC_D0(0xe3, c->insn_lo);
        MOVEC_D0(0xe4, hi2);
    } while (hi2 != c->insn_hi);
    do {
        MOVEC_D0(0xe6, c->arm_hi);
        MOVEC_D0(0xe5, c->arm_lo);
        MOVEC_D0(0xe6, hi2);
    } while (hi2 != c->arm_hi);
    if (ssp)
        UserState(ssp);
}

/* --- the Pi firmware, through mailbox.resource ---------------------- */

/*
 * One property tag with one u32 in and up to two u32s out. The buffer
 * is big-endian here; MB_RawCommand converts, flushes the caches and
 * holds the mailbox's semaphore for the round trip. Static: the
 * daemon's stack is 8 KB and every command shares it.
 */
static ULONG mb_buf[8];

static BOOL mb_get(ULONG tag, ULONG arg, ULONG *v0, ULONG *v1)
{
    if (!g_mb)
        return FALSE;
    mb_buf[0] = 8 * 4;                  /* whole buffer, bytes */
    mb_buf[1] = 0;                      /* a request */
    mb_buf[2] = tag;
    mb_buf[3] = 8;                      /* value buffer: two words */
    mb_buf[4] = 4;                      /* request length */
    mb_buf[5] = arg;
    mb_buf[6] = 0;
    mb_buf[7] = 0;                      /* end tag */
    MB_RawCommand(mb_buf);
    if (mb_buf[1] != 0x80000000UL || !(mb_buf[4] & 0x80000000UL))
        return FALSE;
    if (v0) *v0 = mb_buf[5];
    if (v1) *v1 = mb_buf[6];
    return TRUE;
}

/* --- the report ---------------------------------------------------- */

#define ADD(...) do { \
        if (n < size - 200) n += sprintf(buf + n, __VA_ARGS__); \
    } while (0)

LONG health_report(char *buf, LONG size)
{
    LONG n = 0;
    ULONG a, b;
    char s[160];
    struct E68Counters c;

    probe();

    ADD("mem.chip.free %lu\n", (unsigned long)AvailMem(MEMF_CHIP));
    ADD("mem.chip.largest %lu\n",
        (unsigned long)AvailMem(MEMF_CHIP | MEMF_LARGEST));
    ADD("mem.chip.total %lu\n",
        (unsigned long)AvailMem(MEMF_CHIP | MEMF_TOTAL));
    ADD("mem.fast.free %lu\n", (unsigned long)AvailMem(MEMF_FAST));
    ADD("mem.fast.largest %lu\n",
        (unsigned long)AvailMem(MEMF_FAST | MEMF_LARGEST));
    ADD("mem.fast.total %lu\n",
        (unsigned long)AvailMem(MEMF_FAST | MEMF_TOTAL));

    ADD("emu68 %s\n", g_emu68 ? "yes" : "no");
    if (g_emu68) {
        if (dt_str("/emu68", "idstring", s, sizeof(s)))
            ADD("emu68.version %s\n",
                strncmp(s, "$VER: ", 6) == 0 ? s + 6 : s);
        if (dt_str("/", "model", s, sizeof(s)))
            ADD("pi.model %s\n", s);
        read_counters(&c);
        ADD("emu68.cntfrq %lu\n", (unsigned long)c.frq);
        ADD("emu68.cnt 0x%08lx%08lx\n",
            (unsigned long)c.cnt_hi, (unsigned long)c.cnt_lo);
        ADD("emu68.insn 0x%08lx%08lx\n",
            (unsigned long)c.insn_hi, (unsigned long)c.insn_lo);
        ADD("emu68.arminsn 0x%08lx%08lx\n",
            (unsigned long)c.arm_hi, (unsigned long)c.arm_lo);
        ADD("emu68.jit.size %lu\n", (unsigned long)c.jit_size);
        ADD("emu68.jit.free %lu\n", (unsigned long)c.jit_free);
        ADD("emu68.jit.units %lu\n", (unsigned long)c.jit_count);
        ADD("emu68.jit.misses %lu\n", (unsigned long)c.jit_miss);
    }

    ADD("mailbox %s\n", g_mb ? "yes" : "no");
    if (g_mb) {
        if (mb_get(0x00030006, 0, &a, &b))      /* temperature */
            ADD("pi.temp %lu\n", (unsigned long)b);
        if (mb_get(0x0003000a, 0, &a, &b))      /* max temperature */
            ADD("pi.temp.max %lu\n", (unsigned long)b);
        if (mb_get(0x00030046, 0, &a, NULL))    /* get throttled */
            ADD("pi.throttled 0x%05lx\n", (unsigned long)a);
        if (mb_get(0x00030003, 1, &a, &b))      /* voltage: core */
            ADD("pi.volt.core %lu\n", (unsigned long)b);
        if (mb_get(0x00030003, 2, &a, &b))      /* SDRAM controller */
            ADD("pi.volt.sdram_c %lu\n", (unsigned long)b);
        if (mb_get(0x00030002, 3, &a, &b))      /* ARM clock, set */
            ADD("pi.clock.arm %lu\n", (unsigned long)b);
        if (mb_get(0x00030047, 3, &a, &b))      /* ARM clock, measured */
            ADD("pi.clock.arm.measured %lu\n", (unsigned long)b);
        if (mb_get(0x00030004, 3, &a, &b))      /* ARM clock, max */
            ADD("pi.clock.arm.max %lu\n", (unsigned long)b);
        if (mb_get(0x00030002, 4, &a, &b))      /* core (VPU) clock */
            ADD("pi.clock.core %lu\n", (unsigned long)b);
    }
    return n;
}
