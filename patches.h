/*
 * patches.h - the interface to everything that hijacks a system vector.
 *
 * Every function behind this header exists because it SetFunction()s a
 * library vector and then runs inside whatever task happens to make the
 * call. That is the whole reason this is a separate file: the rules in
 * patches.c are not the daemon's rules, and code written under one set
 * of them must never drift into the other.
 *
 * The boundary is deliberately narrow, and deliberately one-way. The
 * patch layer produces bytes and lines into rings of its own; it knows
 * nothing about sockets, clients or sequence numbers. wasabid.c drains
 * it and decides where the result goes. Nothing below takes a file
 * descriptor, and nothing below should ever start.
 */

#ifndef WASABI_PATCHES_H
#define WASABI_PATCHES_H

#include <exec/types.h>
#include <exec/tasks.h>

/* --- the debug stream: exec's RawPutChar, i.e. every KPrintF byte --- */

/*
 * The daemon's own voice on the debug stream: refusals and connect
 * notices go into the same ring as the machine's KPrintF output, so
 * they arrive in order with it and need no second channel.
 */
void  debug_inject(const char *s);

void  debug_start(void);             /* install the patch, empty the ring */
void  debug_stop(void);              /* remove it, best effort */
LONG  debug_drain(UBYTE *buf, LONG max);   /* bytes waiting; 0 when dry */
ULONG debug_take_lost(void);         /* bytes dropped, and forgets them */

/* --- the snoop stream: the dos.library and exec.library call trace --- */

#define SNOOPF_ENTRY 1               /* also log calls on the way IN */

/* snoop_next_line needs a buffer at least this big; a smaller one is
 * refused rather than truncated, because a half-formatted trace line is
 * worse than no line. */
#define SNOOP_LINE_MAX 320

/*
 * Refuses, with a reason in `why`, when the trampoline and this build
 * disagree - the self-test runs on real hardware before the patches are
 * trusted, and a mismatch means a wild pointer in someone else's task.
 */
BOOL  snoop_start(const char *pat, ULONG flags, char *why, LONG whysz);
void  snoop_stop(void);
LONG  snoop_next_line(char *out, LONG max);   /* one line; 0 when dry */
ULONG snoop_take_lost(void);
UWORD snoop_busy(void);              /* callers still inside a stub */

/* --- the guru report: exec's Alert(), and what survives a reboot --- */

void  guru_arm(void);                /* patch Alert() for the whole run */
void  guru_disarm(void);
void  guru_claim(void);              /* take the black box region */
void  guru_release(void);            /* give it back */
void  guru_boot_check(void);         /* last life's guru -> T:lastguru */
BOOL  guru_take_live(char *out, LONG max);  /* an alert just fired */
BOOL  guru_read_note(char *out, LONG max);  /* T:lastguru, for a new sub */
void  guru_retry_note(void);         /* the deferred write, if T: balked */

/* --- teardown ------------------------------------------------------- */

/*
 * The exit path's one question. A patch that could not be removed is a
 * live pointer into this program's code segment, and returning from
 * main() frees it: the machine then gurus with a privilege violation
 * the next time anything calls through the stale vector. Ask before
 * unloading, and if the answer is TRUE, do not.
 */
BOOL  patches_stuck(void);

/* Who to Signal() when a patch has something urgent (a guru, or an
 * entry-mode line racing a freeze). Set once, from the daemon task. */
void  patches_set_daemon_task(struct Task *t);

/* --- shared utility -------------------------------------------------- */

/* strncpy that always terminates and tolerates a NULL source. Lives
 * here because the patch layer needs a copy that cannot fault; the
 * daemon uses it too rather than keep a second one. */
void  copystr(char *dst, LONG dstsz, const char *src);

#endif /* WASABI_PATCHES_H */
