/*
 * wasabid - the Amiga half of Wasabi.
 *
 * A small daemon that lets a Linux box drive this machine: upload files
 * anywhere, run commands and watch their output arrive live, and reboot.
 * See PROTOCOL.md for the wire format.
 *
 * Build:  make          (Bebbo's m68k-amigaos-gcc)
 * Run:    run >NIL: wasabid
 * Stop:   Break <its CLI number> C
 *
 * This is a remote-code-execution daemon. It belongs on a trusted LAN.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <devices/clipboard.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>       /* CreateExtIO, DeleteExtIO */
#include <proto/dos.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include "wasabi_cgx.h"           /* the CyberGraphX calls, own header */
#define __NOLIBBASE__            /* see g_keymap */
#define KEYMAP_BASE_NAME g_keymap
#include <proto/keymap.h>
#undef __NOLIBBASE__
#include <intuition/classes.h>
#include <libraries/gadtools.h>
#define __NOLIBBASE__            /* opened on the first list in UI */
#define GADTOOLS_BASE_NAME g_gadtools
#include <proto/gadtools.h>
#include <inline/macros.h>              /* LP1: MUI_GetClass, below */
#undef __NOLIBBASE__
#include <rexx/storage.h>
#define __NOLIBBASE__            /* opened on the first AREXX */
#define REXXSYSLIB_BASE_NAME g_rexxsys
#include <proto/rexxsyslib.h>
#undef __NOLIBBASE__

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <proto/bsdsocket.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "patches.h"                 /* everything that hijacks a vector */
#include "health.h"                  /* the machine's vital signs */
#include "tls.h"                     /* every connection is TLS */

#define VERSION_STR "wasabid 0.10"
/* 'used' so the optimizer cannot drop it - C:Version reads this string. */
static const char *verstag __attribute__((used)) =
    "$VER: wasabid 0.10 (15.10.2026)";

#define PROTO_VERSION   1

/*
 * What this build can actually do, sent in WELCOME after the banner.
 *
 * Self-update made version skew an everyday event - the client is a
 * git pull ahead of the daemon until the next `wasabi update` - and
 * "unknown command" is a poor way to learn that. With this the client
 * can say which build is too old and what to do about it.
 *
 * Appending is compatible in both directions: frame lengths are
 * explicit and older clients stop reading after the banner, while a
 * newer client talking to a daemon that sends no list falls back to
 * trying the command and reporting whatever comes back.
 *
 * PROTO_VERSION stays the hard gate, and only for framing changes.
 */
#define CAPS_STR "ping,info,ls,put,get,run,del,mkdir,debug,snoop," \
                 "reboot,restart,ps,kill,speed,speedfile,quit,install," \
                 "grab,screen,hb,guru,snoopentry,psfree,mouse," \
                 "key,windows,health,live,clip,detach,slots,ui,arexx,tls,rexxport,plainopt,pointer,sum,inputs"

/* WELCOME is built in a UBYTE[320]: u16 version, counted banner, counted
 * caps, u32 refused. Growing CAPS_STR past what fits must fail the build
 * here, not scribble past a stack buffer on a machine with no MMU. */
typedef char welcome_fits_its_buffer[
    (2 + 2 + sizeof(VERSION_STR) - 1 +
     2 + sizeof(CAPS_STR) - 1 + 4 <= 320) ? 1 : -1];

#define DEF_PORT        1234
#define MAX_PAYLOAD     65536
#define MAX_CLIENTS     8
#define RUNBUF          4096

/* --- tags (keep in step with PROTOCOL.md) ------------------------- */

#define T_HELLO   0x01
#define T_WELCOME 0x02
#define T_ERR     0x03
#define T_OK      0x04
#define T_PING    0x05
#define T_PONG    0x06
#define T_PUT     0x10
#define T_GET     0x11
#define T_DATA    0x12
#define T_END     0x13
#define T_LS      0x14
#define T_DEL     0x15
#define T_MKDIR   0x16
#define T_RUN     0x20
#define T_STDOUT  0x21
#define T_STDERR  0x22
#define T_EXIT    0x23
#define T_DEBUG   0x30
#define T_SNOOP   0x31
#define T_LOG     0x32
#define T_REBOOT  0x40
#define T_INFO    0x41
#define T_RESTART 0x42
#define T_PS      0x43
#define T_KILL    0x44
#define T_SPEED   0x45
#define T_QUIT    0x46
#define T_INSTALL 0x47
#define T_GRAB    0x48
#define T_SCREEN  0x49
#define T_INPUT   0x4a
#define T_KEY     0x4b
#define T_WINDOWS 0x4c
#define T_HEALTH  0x4d
#define T_LIVE    0x4e
#define T_CLIP    0x4f
#define T_SLOTS   0x50   /* list the run slots */
#define T_FREE    0x51   /* stop a slot's command, or let go of it */
#define T_SLOT    0x52   /* daemon -> client: your RUN is in slot N */
#define T_UI      0x53   /* windows plus gadgets and menus */
#define T_UIACT   0x54   /* pick a menu item; bring a window to front */
#define T_AREXX   0x55   /* send a command to an ARexx port */
#define T_PORTS   0x56   /* list the public message ports */
#define T_SUM     0x57   /* a checksum of a screen region */
#define T_INPUTS  0x58   /* key and mouse events, timed by the Amiga */

struct Library *SocketBase;
/*
 * Opened on demand by `screen` and closed on the way out - a daemon that
 * never grabs one should not hold the graphics stack open.
 *
 * Opened by hand rather than by the startup code on purpose: libnix's
 * auto-open for this library asks for "CyberGfx.library", which is not
 * what it is called on any machine here, and a failed auto-open kills
 * the binary before main() runs. The daemon's own self-test caught that
 * and refused the update, which is the only reason this is a comment
 * and not a machine that needed the keyboard.
 */
static struct Library *g_cgfx;
/*
 * Same rule for keymap.library, opened on the first `key` text: by hand,
 * so a missing library is an ERR on one command, never a binary that
 * will not start.
 */
static struct Library *g_keymap;
static struct Library *g_gadtools;

/* --- the running commands ----------------------------------------- */

/*
 * Up to MAX_RUNS commands run at once, one runner process each, because
 * several sessions share one Amiga and one hung test program used to
 * hold the only slot until a reboot. Each job is its own struct, so the
 * handshake with its runner stays one-to-one.
 *
 * A job the runner will never finish - its command was RemTask'd, or
 * ignored Ctrl-C - can be let go of ('free'): it is "abandoned", counts
 * against no slot, and its struct stays reserved until the runner ends,
 * because the runner still writes into it. MAX_JOBS bounds how many such
 * leftovers can pile up before only a reboot helps.
 */
#define MAX_RUNS 4
#define MAX_JOBS 8

struct RunJob {
    char             cmd[512];
    char             outname[64];
    volatile LONG    rc;
    volatile LONG    ioerr;
    volatile BOOL    done;
    volatile BOOL    taken;
    struct Task * volatile owner;    /* NULLed under Forbid when we leave */
    ULONG            sigmask;
    volatile BPTR    out;            /* the runner's SYS_Output: how its
                                      * command's shell is found */
    BOOL             active;         /* a runner is alive; struct in use */
    BOOL             abandoned;      /* let go of; holds no slot */
    int              client;         /* who gets the output, or -1 */
    BPTR             read;           /* our read end of the temp file */
    LONG             sent;           /* bytes already forwarded */
    ULONG            started;        /* now_secs() at start */
};

static struct RunJob  g_jobs[MAX_JOBS];
static struct RunJob *g_handoff;     /* parent -> runner, one at a time */

static ULONG now_secs(void);
static void force_stop_run(struct RunJob *job);
static BOOL job_wait(struct RunJob *job, LONG ticks);
static void drop(int cl);

/* Commands holding a slot (running, not abandoned). */
static LONG runs_busy(void)
{
    LONG i, n = 0;
    for (i = 0; i < MAX_JOBS; i++)
        if (g_jobs[i].active && !g_jobs[i].abandoned)
            n++;
    return n;
}

/* Any runner alive at all, abandoned or not: our code is in use. */
static BOOL runners_alive(void)
{
    LONG i;
    for (i = 0; i < MAX_JOBS; i++)
        if (g_jobs[i].active)
            return TRUE;
    return FALSE;
}

#define HB_SECS 5                    /* empty-LOG heartbeat cadence */

/*
 * Which client is subscribed to which stream, and where its sequence
 * numbers are up to. This is transport, so it lives here: patches.c
 * produces bytes and lines and has never heard of a socket.
 */
static int   g_dbg_client = -1;
static ULONG g_dbg_seq;
static int   g_snoop_client = -1;
static ULONG g_snoop_seq;
static ULONG g_hb_last;              /* now_secs() of the last heartbeat */

struct Client {
    int   fd;
    BOOL  hello;
    void *tls;                       /* its TLS connection (tls.h) */
    BOOL  secure;                    /* ready for frames: handshake done,
                                      * or a plain loopback client */
    BOOL  local;                     /* from 127.x: may skip TLS */
    ULONG since;                     /* now_secs() at accept */
    /* A transfer in progress, done a piece per main-loop turn so that
     * a big file never makes the other clients wait for all of it. */
    UBYTE xfer;                      /* XF_NONE, XF_PUT, XF_SINK, XF_GET */
    BPTR  xfer_fh;
    ULONG xfer_size, xfer_got, xfer_prot;
    const char *xfer_why;            /* XF_SINK: the error owed at END */
    char  xfer_tmp[300], xfer_path[300];
};
#define XF_NONE 0
#define XF_PUT  1                    /* receiving DATA into xfer_tmp */
#define XF_SINK 2                    /* receiving DATA to throw away */
#define XF_GET  3                    /* sending xfer_fh, a chunk a turn */

static struct Client g_clients[MAX_CLIENTS];
static const char *g_tls_why = "not started";  /* why TLS is not ready */
static BOOL g_allow_plain;           /* 'plain': unencrypted clients too,
                                      * even when TLS is ready */
static ULONG g_tls_tried;                      /* now_secs() of the try */
#define TLS_RETRY_SECS 30
#define HANDSHAKE_SECS 15
static char g_key[128];
static char g_name[32] = "amiga";    /* discovery name: the 'name' startup
                                      * argument, else ENV:HOSTNAME, else
                                      * this default */
static BOOL g_name_set;              /* 'name' given on the command line */
static BOOL g_quit;
static BOOL g_restart;               /* relaunch ourselves on the way out */

/*
 * 'trial': this instance exists only to prove a binary can serve, on a
 * spare port, beside the daemon it is about to replace - and then it
 * exits and its code segment is freed.
 *
 * Two daemons must therefore never both patch the same exec vector.
 * The chain nests correctly on paper (the second one patches over the
 * first and removes itself first), but the cost of being wrong is a
 * jump table entry pointing into freed memory, and the machine then
 * gurus with a privilege violation the moment anything calls through
 * it - somewhere else entirely, minutes later, in a task that had
 * nothing to do with any of this.
 *
 * So a trial instance installs no patches and claims no shared memory
 * at all. It answers a handshake and a ping, which is the entire point
 * of it, and it touches nothing the live daemon owns.
 */
static BOOL g_trial;
static int  g_port = DEF_PORT;        /* the port we listen on */
static char g_extra_args[128];       /* replayed on restart, so a running
                                      * allow-list and name survive a
                                      * self-update */

/* --- who is allowed to talk to us ---------------------------------- */

/*
 * wasabid runs arbitrary commands. The failure that would actually hurt
 * is not a hostile neighbour - it is the daemon being reachable from the
 * internet at all, because somebody forwarded a port, or their router
 * did it for them via UPnP, or the Amiga ended up in a DMZ. So by
 * default we answer only addresses that cannot be routed in from
 * outside: RFC1918 plus loopback.
 *
 * Not narrower than that on purpose. Restricting to 192.168 would lock
 * out every home on an ISP router that hands out 10.0.0.x - Xfinity's
 * default, among others - and buy nothing, because 10/8 and 172.16/12
 * are no more reachable from the internet than 192.168/16 is.
 *
 * Loopback matters more than it looks: a connection arriving through an
 * SSH tunnel terminating on the Amiga comes from 127.0.0.1, so tunnels
 * and this check compose instead of fighting.
 *
 * A mesh VPN (Tailscale hands out 100.64.0.0/10) is a real and sensible
 * way to reach a machine, and is not RFC1918 - hence `allow <cidr>`.
 */
#define MAX_ALLOW 8

struct AllowNet { ULONG base, mask; };
static struct AllowNet g_allow[MAX_ALLOW];
static LONG g_allow_n;
static BOOL g_allow_any;             /* `allow any` - the loaded footgun */

/* Refusals, counted per address rather than logged per event: a port
 * scanner would otherwise write a very large file about one host. */
#define REFUSE_MAX 24

struct Refusal { ULONG ip; ULONG count; };
static struct Refusal g_refused[REFUSE_MAX];
static LONG  g_refused_n;
static ULONG g_refused_total;
static BOOL  g_refused_dirty;
static LONG  g_refused_written = -1; /* ds_Minute of the last file write;
                                      * -1 so minute 0 (midnight) counts */

#define REFUSE_FILE "L:wasabid.refused"

static BOOL parse_cidr(const char *s, struct AllowNet *out)
{
    ULONG oct[4], v = 0;
    LONG bits = 32, i = 0;
    const char *p = s;

    for (i = 0; i < 4; i++) {
        LONG d = 0, n = 0;
        while (*p >= '0' && *p <= '9') { d = d * 10 + (*p++ - '0'); n++; }
        if (!n || d > 255)
            return FALSE;
        oct[i] = (ULONG)d;
        if (i < 3) {
            if (*p != '.') return FALSE;
            p++;
        }
    }
    if (*p == '/') {
        p++;
        bits = 0;
        while (*p >= '0' && *p <= '9') bits = bits * 10 + (*p++ - '0');
        if (bits < 0 || bits > 32) return FALSE;
    }
    if (*p)
        return FALSE;
    v = (oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3];
    out->mask = bits ? (0xFFFFFFFFUL << (32 - bits)) : 0;
    out->base = v & out->mask;
    return TRUE;
}

/* addr is in host byte order. */
static BOOL addr_allowed(ULONG a)
{
    LONG i;
    if (g_allow_any)
        return TRUE;
    if ((a >> 24) == 127)    return TRUE;    /* 127.0.0.0/8    loopback   */
    if ((a >> 24) == 10)     return TRUE;    /* 10.0.0.0/8                */
    if ((a >> 20) == 0xAC1)  return TRUE;    /* 172.16.0.0/12             */
    if ((a >> 16) == 0xC0A8) return TRUE;    /* 192.168.0.0/16            */
    if ((a >> 16) == 0xA9FE) return TRUE;    /* 169.254.0.0/16 link-local */
    for (i = 0; i < g_allow_n; i++)
        if ((a & g_allow[i].mask) == g_allow[i].base)
            return TRUE;
    return FALSE;
}

/*
 * Say who connected, on the debug stream.
 *
 * Counting refusals answers "is anything knocking"; it does not answer
 * "who is actually talking to my Amiga", which is the more useful
 * question once a VPN subnet router is in the picture - traffic from a
 * whole tailnet arrives wearing the router's LAN address, and only the
 * accept path ever sees it.
 */
static void note_accept(ULONG a)
{
    char line[64];
    sprintf(line, "[wasabi: %lu.%lu.%lu.%lu connected]\n",
            (unsigned long)((a >> 24) & 255), (unsigned long)((a >> 16) & 255),
            (unsigned long)((a >> 8) & 255), (unsigned long)(a & 255));
    debug_inject(line);
}

static void note_refusal(ULONG a)
{
    char line[80];
    LONG i, worst = 0;

    g_refused_total++;
    g_refused_dirty = TRUE;

    for (i = 0; i < g_refused_n; i++)
        if (g_refused[i].ip == a) {
            g_refused[i].count++;
            goto said;
        }
    if (g_refused_n < REFUSE_MAX) {
        g_refused[g_refused_n].ip = a;
        g_refused[g_refused_n].count = 1;
        g_refused_n++;
    } else {
        /* Full: replace the quietest, so persistent knockers survive. */
        for (i = 1; i < REFUSE_MAX; i++)
            if (g_refused[i].count < g_refused[worst].count)
                worst = i;
        g_refused[worst].ip = a;
        g_refused[worst].count = 1;
    }
said:
    sprintf(line, "[wasabi: refused %lu.%lu.%lu.%lu - not on the LAN]\n",
            (unsigned long)((a >> 24) & 255), (unsigned long)((a >> 16) & 255),
            (unsigned long)((a >> 8) & 255), (unsigned long)(a & 255));
    debug_inject(line);
}

/*
 * Write the whole (small) table, not an append: it is a tally, and one
 * rewrite of a couple of hundred bytes is cheaper than an ever-growing
 * file nobody reads. Called from the main loop and rate-limited to once
 * a minute, so a scan in progress cannot turn into disk thrash.
 */
static void refusals_save(BOOL force)
{
    struct DateStamp now;
    BPTR fh;
    LONG i;

    if (!g_refused_dirty)
        return;
    DateStamp(&now);
    if (!force && now.ds_Minute == g_refused_written)
        return;
    fh = Open(REFUSE_FILE, MODE_NEWFILE);
    if (!fh)
        return;                      /* not worth failing the daemon over */
    {
        char line[80];
        LONG n = sprintf(line, "total %lu\n", (unsigned long)g_refused_total);
        Write(fh, line, n);
        for (i = 0; i < g_refused_n; i++) {
            ULONG a = g_refused[i].ip;
            n = sprintf(line, "%lu.%lu.%lu.%lu %lu\n",
                        (unsigned long)((a >> 24) & 255),
                        (unsigned long)((a >> 16) & 255),
                        (unsigned long)((a >> 8) & 255),
                        (unsigned long)(a & 255),
                        (unsigned long)g_refused[i].count);
            Write(fh, line, n);
        }
    }
    Close(fh);
    g_refused_written = now.ds_Minute;
    g_refused_dirty = FALSE;
}

/* Pick the running total back up across a restart or a reboot. */
static void refusals_load(void)
{
    BPTR fh = Open(REFUSE_FILE, MODE_OLDFILE);
    char line[80];
    if (!fh)
        return;
    if (FGets(fh, line, sizeof(line) - 1)) {
        ULONG t = 0;
        const char *p = line;
        while (*p && (*p < '0' || *p > '9')) p++;
        while (*p >= '0' && *p <= '9') t = t * 10 + (*p++ - '0');
        g_refused_total = t;
    }
    Close(fh);
}

/* --- byte order helpers ------------------------------------------- */

static void put_be32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);  p[3] = (UBYTE)v;
}

static ULONG get_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static UWORD get_be16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | p[1]);
}

/* --- framing ------------------------------------------------------- */

/*
 * Every blocking read and write in the daemon waits here first, and that
 * is the whole point: wasabid is one process with one loop, so a single
 * wedged peer blocking in recv() or send() takes the machine with it -
 * no other client served, no stream pumped, no Ctrl-C honoured, nothing
 * to do but walk to the Amiga.
 *
 * Two peers can do it. One sends half a frame and stalls; one subscribes
 * to the debug stream and stops reading until the socket buffers fill.
 * Both used to be forever. Now both are ten seconds, and then that
 * client is dropped - the rest of the machine never notices.
 *
 * Ten seconds is enormous next to an honest frame: 64 KB crosses this
 * network in under a millisecond, and the client sends a frame's body
 * straight after its header. Nothing legitimate waits that long.
 *
 * CTRL-C is watched alongside the socket, so a wedged transfer can never
 * make the daemon unkillable from its own keyboard. The signal is
 * consumed here, so it is turned into g_quit for the main loop to see.
 */
#define IO_TIMEOUT_SECS 10

static BOOL io_wait(int fd, BOOL forwrite)
{
    fd_set set;
    struct timeval tv;
    ULONG sigs = SIGBREAKF_CTRL_C;
    LONG n;

    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_secs  = IO_TIMEOUT_SECS;
    tv.tv_micro = 0;
    n = WaitSelect(fd + 1, forwrite ? NULL : &set, forwrite ? &set : NULL,
                   NULL, &tv, &sigs);
    if (sigs & SIGBREAKF_CTRL_C) {
        g_quit = TRUE;
        return FALSE;
    }
    return n > 0;                    /* 0 = timed out, <0 = the socket died */
}

/* The TLS connection behind a client socket. Every client has one once
 * accepted; the lookup is by fd because the whole daemon passes fds. */
static void *tls_of(int fd)
{
    LONG i;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].fd == fd)
            return g_clients[i].tls;
    return NULL;
}

/*
 * TLS on a non-blocking socket: try, and when it wants the socket wait
 * for exactly that, with the same ten-second bound as plain I/O. A TLS
 * write must be retried with the same bytes, so it loops here rather
 * than returning half done.
 */
static BOOL tls_send_all(int fd, void *t, const UBYTE *buf, LONG len)
{
    while (len > 0) {
        LONG n = tls_write(t, buf, len);
        if (n > 0) {
            buf += n;
            len -= n;
        } else if (n == 0 || n == -2) {
            if (!io_wait(fd, n == 0))
                return FALSE;
        } else
            return FALSE;
    }
    return TRUE;
}

static BOOL tls_recv_all(int fd, void *t, UBYTE *buf, LONG len)
{
    while (len > 0) {
        LONG n = tls_read(t, buf, len);
        if (n > 0) {
            buf += n;
            len -= n;
        } else if (n == 0 || n == -2) {
            if (!io_wait(fd, n == -2))
                return FALSE;
        } else
            return FALSE;
    }
    return TRUE;
}

static BOOL send_all(int fd, const UBYTE *buf, LONG len)
{
    void *t = tls_of(fd);
    if (t)
        return tls_send_all(fd, t, buf, len);
    while (len > 0) {
        LONG n;
        if (!io_wait(fd, TRUE))
            return FALSE;
        n = send(fd, (void *)buf, len, 0);
        if (n <= 0)
            return FALSE;
        buf += n;
        len -= n;
    }
    return TRUE;
}

static BOOL send_frame(int fd, UBYTE tag, const void *payload, LONG len)
{
    UBYTE hdr[5];
    void *t = tls_of(fd);
    if (t) {
        /* One record for header and body: two would double the TLS
         * overhead on every small frame, and the live view and streams
         * send a great many. Static: 64 KB is no stack's business. */
        static UBYTE frame[5 + MAX_PAYLOAD];
        if (len < 0 || len > MAX_PAYLOAD)
            return FALSE;
        frame[0] = tag;
        put_be32(frame + 1, (ULONG)len);
        if (len)
            memcpy(frame + 5, payload, len);
        return tls_send_all(fd, t, frame, 5 + len);
    }
    hdr[0] = tag;
    put_be32(hdr + 1, (ULONG)len);
    if (!send_all(fd, hdr, 5))
        return FALSE;
    return len ? send_all(fd, (const UBYTE *)payload, len) : TRUE;
}

static BOOL recv_all(int fd, UBYTE *buf, LONG len)
{
    void *t = tls_of(fd);
    if (t)
        return tls_recv_all(fd, t, buf, len);
    while (len > 0) {
        LONG n;
        if (!io_wait(fd, FALSE))
            return FALSE;
        n = recv(fd, (void *)buf, len, 0);
        if (n <= 0)
            return FALSE;
        buf += n;
        len -= n;
    }
    return TRUE;
}

/*
 * Read one whole frame, in the caller's own time rather than the main
 * loop's: PUT and SPEED read their bodies straight through this. A peer
 * that stops mid-frame is bounded by io_wait() and then dropped, so the
 * daemon stays the daemon.
 *
 * This is still not a state machine - one client is served at a time
 * while its frame arrives, which is fine for one developer and one
 * Amiga. Multi-user would want a per-client input buffer instead.
 */
static BOOL recv_frame(int fd, UBYTE *tag, UBYTE *payload, LONG *len)
{
    UBYTE hdr[5];
    ULONG n;

    if (!recv_all(fd, hdr, 5))
        return FALSE;
    n = get_be32(hdr + 1);
    if (n > MAX_PAYLOAD)
        return FALSE;            /* refuse to size an allocation from the wire */
    if (n && !recv_all(fd, payload, (LONG)n))
        return FALSE;
    *tag = hdr[0];
    *len = (LONG)n;
    return TRUE;
}

static BOOL send_err_full(int fd, ULONG code, const char *msg)
{
    UBYTE buf[256];
    LONG mlen = (LONG)strlen(msg);
    if (mlen > 200) mlen = 200;
    put_be32(buf, code);
    buf[4] = (UBYTE)(mlen >> 8);
    buf[5] = (UBYTE)mlen;
    memcpy(buf + 6, msg, mlen);
    return send_frame(fd, T_ERR, buf, 6 + mlen);
}

/* A DOS call just failed: IoErr() is fresh and belongs to this error. */
static BOOL send_err(int fd, const char *msg)
{
    return send_err_full(fd, (ULONG)IoErr(), msg);
}

/* Our own refusal - bad frame, bad key, busy slot. No DOS call failed,
 * so a leftover IoErr() would only dress the message in an unrelated
 * AmigaDOS error number the client then dutifully prints. */
static BOOL send_perr(int fd, const char *msg)
{
    return send_err_full(fd, 0, msg);
}

/* Pull a wire string (u16 len + bytes) out of a payload, NUL-terminating. */
static BOOL get_str(const UBYTE *p, LONG len, LONG off, char *out, LONG outsz)
{
    LONG n;
    if (off + 2 > len)
        return FALSE;
    n = get_be16(p + off);
    if (off + 2 + n > len || n >= outsz)
        return FALSE;
    memcpy(out, p + off + 2, n);
    out[n] = '\0';
    return TRUE;
}

/* --- the runner process -------------------------------------------- */

/*
 * Runs one command with its output redirected to a temp file which the
 * daemon tails. The file is opened MODE_READWRITE, not MODE_NEWFILE,
 * because MODE_NEWFILE takes an EXCLUSIVE lock and the daemon could then
 * never open it to read. That one flag is the whole trick.
 */
static void runner_entry(void)
{
    struct RunJob *job = g_handoff;
    BPTR out, in;

    job->taken = TRUE;                 /* release the parent */

    /*
     * No requesters. There is nobody at that keyboard, so "Please insert
     * volume AmiSSL: in any drive" is not a question - it is a wedge:
     * the runner blocks inside SystemTags() forever, taking the daemon's
     * single run slot and 128 KB of stack with it, and only someone
     * physically at the machine can clear it. With this, DOS fails the
     * call instead and the client gets an error it can act on.
     */
    ((struct Process *)FindTask(NULL))->pr_WindowPtr = (APTR)-1;

    out = Open(job->outname, MODE_READWRITE);
    in  = Open("NIL:", MODE_OLDFILE);
    job->out = out;
    if (out && in) {
        job->rc = SystemTags(job->cmd,
                             SYS_Input,  (ULONG)in,
                             SYS_Output, (ULONG)out,
                             SYS_UserShell, TRUE,
                             TAG_DONE);
        job->ioerr = IoErr();
    } else {
        job->rc = 20;
        job->ioerr = IoErr();
    }
    if (out) Close(out);
    if (in)  Close(in);

    /*
     * Finish inside Forbid and never Permit: the process ends here, so
     * once the daemon can see done, not one more instruction of ours
     * runs in this process - the daemon may unload the segment at once.
     * owner is NULL when the daemon has already left (it kept the
     * segment loaded for us; see leave_runners_behind).
     */
    Forbid();
    job->out = 0;
    job->done = TRUE;
    if (job->owner)
        Signal(job->owner, job->sigmask);
}

static BOOL send_err_full(int fd, ULONG code, const char *msg);

/* Start cmd in a free slot. Returns the job, or NULL with *why set. */
static struct RunJob *start_run(int cl, const char *cmd, const char **why)
{
    static LONG serial;
    struct Process *proc;
    struct RunJob *job = NULL;
    LONG i;

    for (i = 0; i < MAX_JOBS; i++)
        if (g_jobs[i].active && g_jobs[i].client == cl &&
            !g_jobs[i].abandoned) {
            *why = "this connection already has a command running";
            return NULL;
        }
    if (runs_busy() >= MAX_RUNS) {
        *why = "all run slots are busy - 'wasabi slots' shows them, "
               "'wasabi free N' stops one";
        return NULL;
    }
    for (i = 0; i < MAX_JOBS && !job; i++)
        if (!g_jobs[i].active)
            job = &g_jobs[i];
    if (!job) {
        *why = "too many stuck commands are still holding memory - "
               "only a reboot clears them";
        return NULL;
    }
    *why = "could not start the command";

    memset(job, 0, sizeof(*job));
    job->client = -1;
    strncpy(job->cmd, cmd, sizeof(job->cmd) - 1);
    sprintf(job->outname, "T:wasabi-run-%ld", (long)++serial);
    job->owner   = FindTask(NULL);
    job->sigmask = SIGBREAKF_CTRL_F;

    /* Create the file up front so our own tail can open it immediately. */
    {
        BPTR seed = Open(job->outname, MODE_NEWFILE);
        if (!seed)
            return NULL;
        Close(seed);
    }

    g_handoff = job;
    proc = CreateNewProcTags(NP_Entry,     (ULONG)runner_entry,
                             NP_Name,      (ULONG)"wasabi-runner",
                             NP_StackSize, 16384,
                             NP_Cli,       TRUE,
                             TAG_DONE);
    if (!proc) {
        g_handoff = NULL;
        DeleteFile(job->outname);
        return NULL;
    }
    /* Wait for the runner to pick the job up before reusing the handoff. */
    while (!job->taken)
        Delay(1);
    g_handoff = NULL;

    job->read    = Open(job->outname, MODE_OLDFILE);
    job->sent    = 0;
    job->client  = cl;
    job->started = now_secs();
    job->active  = TRUE;
    return job;
}

/*
 * Read from the tail handle, seeing through ram-handler's blind spot.
 *
 * A ram-handler file handle anchors to the file's data at Open() time.
 * Opened while the file is still empty, it stays blind forever: bytes
 * another handle appends later never show - not after the writer's
 * Flush(), not after its Close(), not even after Seek() on the reader.
 * The tail handle is opened moments after the runner starts, so any
 * command that took longer than that to produce its first byte -
 * anything that sleeps before writing, or buffers stdio and flushes at
 * exit - came back with its entire output missing. A handle opened on
 * a file that already has data follows later growth fine (measured on
 * OS 3.2), which is why fast starters always worked and the failures
 * looked like they were about sleeping.
 *
 * ExamineFH() on the stale handle still reports the file's true size,
 * so falling behind is detectable: more bytes in the file than we have
 * forwarded. The cure is a fresh Open() - a new handle sees the
 * current data - resumed where the old one left off. The fresh handle
 * is anchored to real data, so one reopen heals the stream for good.
 *
 * Accounting lives here, not in the callers: job->sent is the resume
 * point after a reopen, and a caller that forgot to add to it (the
 * final sweep once did not) would make a reopen resend those bytes.
 */
static LONG tail_read(struct RunJob *job, UBYTE *buf, LONG len)
{
    /* Longword aligned as ExamineFH demands; static because this runs
     * on the daemon's 8 KB shell stack like everything around it. */
    static struct FileInfoBlock fib __attribute__((aligned(4)));
    LONG n = Read(job->read, buf, len);

    if (n == 0 && ExamineFH(job->read, &fib) && fib.fib_Size > job->sent) {
        BPTR fresh = Open(job->outname, MODE_OLDFILE);
        if (fresh) {
            Close(job->read);
            job->read = fresh;
            Seek(fresh, job->sent, OFFSET_BEGINNING);
            n = Read(fresh, buf, len);
        }
    }
    if (n > 0)
        job->sent += n;
    return n;
}

/* Forward whatever the child has flushed. Returns FALSE if the client died.
 * Also runs headless (client == -1) after the client hung up or the job
 * was abandoned, so the runner's temp file still gets cleaned up and the
 * struct freed on done. */
static BOOL pump_run(struct RunJob *job)
{
    /* Static for the same reason as send_log's and the stream pumps':
     * this runs below serve() on the force-quit path, and 4 KB of
     * automatic here is what tips that chain over the 8 KB stack.
     * One task, never re-entered - the main loop cannot be pumping
     * while serve() is. */
    static UBYTE buf[RUNBUF];
    int fd = (job->client >= 0) ? g_clients[job->client].fd : -1;
    LONG n;

    if (job->read && fd >= 0) {
        while ((n = tail_read(job, buf, sizeof(buf))) > 0) {
            if (!send_frame(fd, T_STDOUT, buf, n))
                return FALSE;
        }
    }
    if (job->done) {
        UBYTE ex[8];
        /* One last sweep: the child may have flushed as it exited. */
        if (job->read) {
            if (fd >= 0) {
                while ((n = tail_read(job, buf, sizeof(buf))) > 0)
                    if (!send_frame(fd, T_STDOUT, buf, n))
                        return FALSE;
            }
            Close(job->read);
            job->read = 0;
        }
        DeleteFile(job->outname);
        job->active = FALSE;             /* the runner is gone; slot free */
        if (fd >= 0) {
            put_be32(ex, (ULONG)job->rc);
            put_be32(ex + 4, (ULONG)job->ioerr);
            /* Send EXIT while job->client is still valid: clearing it
             * first meant a failed send made the main loop drop(-1) -
             * an out-of-bounds write into whatever sits before the
             * client table. */
            if (!send_frame(fd, T_EXIT, ex, 8))
                return FALSE;
        }
        job->client = -1;
    }
    return TRUE;
}

/*
 * The Shell process running a job's command. SystemTags() runs it in a
 * Background CLI of its own, whose output stream is the handle the
 * runner passed as SYS_Output - which is how it is told apart from every
 * other Shell, including another slot running the same command. Call
 * under Forbid(); NULL while the Shell has not started yet, or when the
 * command's process is gone.
 */
static struct Process *job_shell(struct RunJob *job)
{
    LONG i, max;
    if (!job->out)
        return NULL;
    max = MaxCli();
    for (i = 1; i <= max; i++) {
        struct Process *p = FindCliProc(i);
        if (p && p->pr_COS == job->out)
            return p;
    }
    return NULL;
}

/* Ctrl-C a job's command. TRUE if its Shell was found and signalled. */
static BOOL job_break(struct RunJob *job)
{
    struct Process *p;
    Forbid();
    p = job_shell(job);
    if (p)
        Signal((struct Task *)p, SIGBREAKF_CTRL_C);
    Permit();
    return p != NULL;
}

/*
 * Let go of a job whose runner will not end: its client hears that the
 * slot was freed (as an ERR, which every client already handles mid-RUN)
 * and the slot counts as free. The runner may still end one day; then
 * pump_run frees the struct quietly.
 */
static void job_abandon(struct RunJob *job, const char *why)
{
    if (job->client >= 0)
        send_err_full(g_clients[job->client].fd, 0, why);
    if (job->read) { Close(job->read); job->read = 0; }
    job->client = -1;
    job->abandoned = TRUE;
}

/*
 * On the way out with runners that will not end: they execute this
 * segment's code and write into g_jobs, which lives in it too. So tell
 * them nobody is listening (owner NULL - under Forbid, because the
 * runner's last act reads it there), and take the segment away from the
 * Shell that would unload it after main returns - the same trick that
 * detaching startup code plays. It costs this copy of wasabid in memory
 * until a reboot - about 310 KB, most of it the live view's buffers
 * (measured on the A1200, 0.4b1); a Guru minutes later costs more.
 */
static void leave_runners_behind(void)
{
    struct CommandLineInterface *cli;
    LONG i, n = 0;

    Forbid();
    for (i = 0; i < MAX_JOBS; i++)
        if (g_jobs[i].active && !g_jobs[i].done) {
            g_jobs[i].owner = NULL;
            n++;
        }
    Permit();
    if (!n)
        return;
    cli = Cli();
    if (cli && cli->cli_Module) {
        cli->cli_Module = 0;
        Printf("wasabid: %ld stuck command(s) left running; this copy of "
               "wasabid stays in memory for them until a reboot\n", n);
        return;
    }
    /* Not started from a Shell, so nothing to take the segment from:
     * all that is safe is to wait. */
    Printf("wasabid: %ld stuck command(s) still running - waiting for "
           "them before exiting\n", n);
    for (i = 0; i < MAX_JOBS; i++)
        while (g_jobs[i].active && !g_jobs[i].done)
            Delay(50);
}

/*
 * SLOTS: one line per job, "<slot> <state> <secs> <client> <cmd>\n",
 * state running|stuck (abandoned), client 1 when someone is reading the
 * output. Same DATA..END shape as ps.
 */
static BOOL cmd_slots(int fd)
{
    static char line[600];               /* static: the 8 KB shell stack */
    ULONG now = now_secs();
    LONG i;
    for (i = 0; i < MAX_JOBS; i++) {
        struct RunJob *job = &g_jobs[i];
        LONG ln;
        if (!job->active)
            continue;
        ln = sprintf(line, "%ld %s %lu %d %s\n", (long)i + 1,
                     job->abandoned ? "stuck" : "running",
                     (unsigned long)(now - job->started),
                     job->client >= 0 ? 1 : 0, job->cmd);
        if (!send_frame(fd, T_DATA, line, ln))
            return FALSE;
    }
    return send_frame(fd, T_END, NULL, 0);
}

/*
 * FREE <u32 slot> <u32 flags>: Ctrl-C the slot's command and wait up to
 * three seconds. If it ended, its client gets the EXIT as usual. If not,
 * and FREE_BREAK_ONLY is clear, let go of it: the slot is free again and
 * the command is left running. Answers DATA "stopped" / "freed" / "asked"
 * then END - or ERR.
 */
#define FREE_BREAK_ONLY 1

static BOOL cmd_free(int fd, ULONG slot, ULONG flags)
{
    struct RunJob *job;
    const char *what;
    if (slot < 1 || slot > MAX_JOBS || !g_jobs[slot - 1].active)
        return send_perr(fd, "no command in that slot - 'wasabi slots' "
                             "lists them");
    job = &g_jobs[slot - 1];
    if (job->abandoned)
        return send_perr(fd, "that slot was already freed; its command "
                             "still runs and ends on its own or at a "
                             "reboot");
    force_stop_run(job);
    if (flags & FREE_BREAK_ONLY)
        what = "asked";
    else if (job_wait(job, 150)) {
        what = "stopped";
        if (!pump_run(job) && job->client >= 0)
            drop(job->client);
    } else {
        what = "freed";
        job_abandon(job, "the command was let go of with 'wasabi free' - "
                         "it may still be running on the Amiga");
    }
    if (!send_frame(fd, T_DATA, (UBYTE *)what, (LONG)strlen(what)))
        return FALSE;
    return send_frame(fd, T_END, NULL, 0);
}

/* --- the debug stream ---------------------------------------------- */

static BOOL send_log(int fd, ULONG stream, ULONG seq,
                     const UBYTE *text, LONG len)
{
    /* Static, not automatic: this daemon runs on the shell's 8 KB
     * stack, and 4 KB of frame here plus 4 KB in whatever is draining
     * into it is enough to run off the end. Single-threaded, one task,
     * never re-entered - so a static costs nothing and cannot be the
     * thing that smashes memory somewhere else entirely. */
    static UBYTE pl[10 + RUNBUF];
    if (len > RUNBUF)
        len = RUNBUF;
    put_be32(pl, stream);
    put_be32(pl + 4, seq);
    pl[8] = (UBYTE)(len >> 8);
    pl[9] = (UBYTE)len;
    memcpy(pl + 10, text, len);
    return send_frame(fd, T_LOG, pl, 10 + len);
}

/*
 * Ship what the debug patch captured. The buffer is static for the same
 * reason send_log's is: an 8 KB stack does not hold two 4 KB frames.
 */
static BOOL pump_debug_stream(void)
{
    static UBYTE buf[RUNBUF];
    int fd = g_clients[g_dbg_client].fd;
    ULONG lost;
    LONG n;

    while ((n = debug_drain(buf, sizeof(buf))) > 0)
        if (!send_log(fd, 0, ++g_dbg_seq, buf, n))
            return FALSE;
    if ((lost = debug_take_lost()) != 0) {
        char note[64];
        LONG ln = sprintf(note, "\n[wasabi: %lu debug byte(s) lost]\n",
                          (unsigned long)lost);
        if (!send_log(fd, 0, ++g_dbg_seq, (UBYTE *)note, ln))
            return FALSE;
    }
    return TRUE;
}

static BOOL pump_snoop_stream(void)
{
    static char line[SNOOP_LINE_MAX];
    int fd = g_clients[g_snoop_client].fd;
    ULONG lost;
    LONG n;

    while ((n = snoop_next_line(line, sizeof(line))) > 0)
        if (!send_log(fd, 1, ++g_snoop_seq, (UBYTE *)line, n))
            return FALSE;
    if ((lost = snoop_take_lost()) != 0) {
        char note[64];
        LONG ln = sprintf(note, "[wasabi: %lu snoop event(s) lost]\n",
                          (unsigned long)lost);
        if (!send_log(fd, 1, ++g_snoop_seq, (UBYTE *)note, ln))
            return FALSE;
    }
    return TRUE;
}

/*
 * A newly subscribed stream opens with the last guru, if the machine
 * has one to report. patches.c keeps the note; this puts it on the
 * wire.
 */
static void stream_greet(int cl, ULONG stream, ULONG *seq)
{
    char note[384];                      /* the note is up to 319 + the
                                          * "[wasabi: last guru: ]" wrap */
    if (guru_read_note(note, sizeof(note)))
        send_log(g_clients[cl].fd, stream, ++(*seq),
                 (UBYTE *)note, (LONG)strlen(note));
}

/* Seconds since the Amiga epoch, for pacing the stream heartbeat. */
static ULONG now_secs(void)
{
    struct DateStamp d;
    DateStamp(&d);
    return (ULONG)d.ds_Days * 86400UL + (ULONG)d.ds_Minute * 60UL +
           (ULONG)d.ds_Tick / 50UL;
}

/* An empty heartbeat LOG, sent only if the socket will take it without
 * waiting. Skipping is TRUE - congested is not dead; see the heartbeat
 * comment in the main loop. FALSE means the send itself failed. */
static BOOL hb_send(int fd, ULONG stream, ULONG *seq)
{
    fd_set w;
    struct timeval tv;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    tv.tv_secs = 0;
    tv.tv_micro = 0;
    if (WaitSelect(fd + 1, NULL, &w, NULL, &tv, NULL) <= 0)
        return TRUE;
    return send_log(fd, stream, ++(*seq), (const UBYTE *)"", 0);
}

/* One visible line on every open stream, in the same voice as the
 * connect/refusal notices. A failed send is not a reason to drop the
 * subscriber here - these lines are best-effort by nature. */
static void stream_note(const char *line, LONG n)
{
    if (g_dbg_client >= 0)
        send_log(g_clients[g_dbg_client].fd, 0, ++g_dbg_seq,
                 (UBYTE *)line, n);
    if (g_snoop_client >= 0)
        send_log(g_clients[g_snoop_client].fd, 1, ++g_snoop_seq,
                 (UBYTE *)line, n);
}

/*
 * The daemon is going down on purpose - reboot, restart, quit, Break.
 * Say so on any open stream first: to the operator watching `wasabi
 * debug`, a machine that stops streaming and one that froze look
 * identical, and only the daemon knows which this is.
 */
static void say_goodbye(const char *why)
{
    char line[80];
    LONG n = sprintf(line, "[wasabi: %s - closing this stream]\n", why);
    stream_note(line, n);
}

/* --- ps and kill --------------------------------------------------- */

/*
 * A snapshot of every task on the machine: ThisTask plus the TaskReady
 * and TaskWait lists. Those lists are the scheduler's working state, so
 * the walk happens under Disable() and copies everything out - a task
 * pointer is only trustworthy while interrupts stay off.
 */
struct PsEnt {
    APTR  addr;
    char  kind;                      /* 'p'rocess or 't'ask */
    BYTE  pri;
    UBYTE state;                     /* 0 run, 1 ready, 2 wait */
    ULONG stack;
    LONG  free;                      /* bytes of headroom, -1 unknown */
    LONG  cli;                       /* CLI number, -1 when none */
    char  name[48];
    char  cmd[48];                   /* CLI command in flight, if any */
};

#define PS_MAX 128

static struct PsEnt g_ps[PS_MAX];

/*
 * Headroom, which is the number that predicts a crash - capacity only
 * tells you what the task was given, not what it has left. This daemon
 * has been bitten four times by a large automatic on an 8 KB stack
 * (docs/audits/audit2.md), and every one of those was found by reading source on
 * the Linux box, because nothing on the wire could show it.
 *
 * Two ways to be wrong, both handled:
 *
 *  - tc_SPReg is only meaningful for a task the scheduler has parked.
 *    For the RUNNING task it is whatever was saved last time, which is
 *    stale by definition - so the caller passes the real stack pointer
 *    for that one entry (see ps_collect) rather than a plausible lie.
 *
 *  - A CLI program may swap stacks, leaving sp outside the Task's own
 *    bounds. That is not an error and not a measurement either: it is
 *    reported as unknown rather than as a huge or negative number.
 */
static LONG stack_free(struct Task *t, APTR sp_now)
{
    APTR sp = sp_now ? sp_now : (APTR)t->tc_SPReg;
    if (sp <= t->tc_SPLower || sp > t->tc_SPUpper)
        return -1;
    return (LONG)((char *)sp - (char *)t->tc_SPLower);
}

static void ps_add(LONG *n, struct Task *t, UBYTE state, APTR sp_now)
{
    struct PsEnt *e;
    if (*n >= PS_MAX)
        return;
    e = &g_ps[(*n)++];
    e->addr  = t;
    e->kind  = t->tc_Node.ln_Type == NT_PROCESS ? 'p' : 't';
    e->pri   = t->tc_Node.ln_Pri;
    e->state = state;
    e->stack = (ULONG)((char *)t->tc_SPUpper - (char *)t->tc_SPLower);
    e->free  = stack_free(t, sp_now);
    e->cli   = -1;
    e->cmd[0] = '\0';
    copystr(e->name, sizeof(e->name), t->tc_Node.ln_Name);
    if (e->kind == 'p') {
        struct Process *pr = (struct Process *)t;
        struct CommandLineInterface *cli =
            (struct CommandLineInterface *)BADDR(pr->pr_CLI);
        if (pr->pr_TaskNum > 0)
            e->cli = pr->pr_TaskNum;
        if (cli) {
            UBYTE *b = (UBYTE *)BADDR(cli->cli_CommandName);
            if (b && b[0]) {                 /* a BSTR: length, then bytes */
                LONG bn = b[0];
                if (bn > (LONG)sizeof(e->cmd) - 1)
                    bn = sizeof(e->cmd) - 1;
                memcpy(e->cmd, b + 1, bn);
                e->cmd[bn] = '\0';
            }
        }
    }
}

static LONG ps_collect(void)
{
    LONG n = 0;
    struct Task *t;
    char here;                       /* its address is this task's sp */

    Disable();
    /* ThisTask is always us - we are the one serving the request - and
     * our own tc_SPReg is stale, so measure the stack we are standing
     * on instead of reading the register the scheduler has not updated
     * since we last gave up the CPU. */
    ps_add(&n, SysBase->ThisTask, 0, (APTR)&here);
    for (t = (struct Task *)SysBase->TaskReady.lh_Head;
         t->tc_Node.ln_Succ; t = (struct Task *)t->tc_Node.ln_Succ)
        ps_add(&n, t, 1, NULL);
    for (t = (struct Task *)SysBase->TaskWait.lh_Head;
         t->tc_Node.ln_Succ; t = (struct Task *)t->tc_Node.ln_Succ)
        ps_add(&n, t, 2, NULL);
    Enable();
    return n;
}

/*
 * PSF_FREE is asked for by the client, not decided here, and that is
 * what keeps the wire compatible in both directions: an older daemon
 * never reads the payload at all, and an older client never sends one,
 * so it gets the seven-field line it has always parsed. A field added
 * unconditionally would have shifted <name> and broken every client in
 * the field - the line has no room to grow at the end either, because
 * <cmd> is the tab-delimited remainder.
 */
#define PSF_FREE 1

static BOOL cmd_ps(int fd, ULONG flags)
{
    static const char * const statename[] = { "run", "ready", "wait" };
    char line[176];
    LONG n = ps_collect(), i;

    for (i = 0; i < n; i++) {
        struct PsEnt *e = &g_ps[i];
        LONG ln;
        if (flags & PSF_FREE)
            ln = sprintf(line, "0x%08lx %c %ld %s %lu %ld %ld %s\t%s\n",
                         (unsigned long)e->addr, e->kind, (long)e->pri,
                         statename[e->state], (unsigned long)e->stack,
                         (long)e->free, (long)e->cli, e->name, e->cmd);
        else
            ln = sprintf(line, "0x%08lx %c %ld %s %lu %ld %s\t%s\n",
                         (unsigned long)e->addr, e->kind, (long)e->pri,
                         statename[e->state], (unsigned long)e->stack,
                         (long)e->cli, e->name, e->cmd);
        if (!send_frame(fd, T_DATA, line, ln))
            return FALSE;
    }
    return send_frame(fd, T_END, NULL, 0);
}

/* Case-insensitive whole-string compare; pure, so safe under Disable. */
static BOOL str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return FALSE;
        a++; b++;
    }
    return *a == *b;
}

/*
 * Find the task named (or addressed) by target and either Signal() it
 * CTRL-C - what the Break command does - or RemTask() it outright.
 * The target must match exactly one task; a name is matched against
 * both the task name and the CLI command it is running. The action
 * happens under Disable() after re-finding the task in the lists, so a
 * target that exited since ps cannot be a stale pointer. RemTask frees
 * none of the locks, semaphores or DOS state the task holds - it is the
 * last resort the --force flag says it is.
 */
static BOOL cmd_kill(int fd, ULONG flags, const char *target)
{
    struct Task *hit = NULL;
    struct RunJob *orphan = NULL;
    APTR addr = NULL;
    LONG matches = 0, n, i;
    BOOL alive = FALSE;

    if (target[0] == '0' && (target[1] == 'x' || target[1] == 'X'))
        addr = (APTR)strtoul(target, NULL, 16);

    n = ps_collect();
    for (i = 0; i < n; i++) {
        struct PsEnt *e = &g_ps[i];
        if (addr ? (e->addr == addr)
                 : (str_ieq(e->name, target) ||
                    (e->cmd[0] && str_ieq(e->cmd, target)))) {
            matches++;
            hit = (struct Task *)e->addr;
        }
    }
    if (!matches)
        return send_perr(fd, "no task or process by that name");
    if (matches > 1)
        /* send_perr, not send_err: ps_collect() makes no DOS call, so
         * IoErr() here still belongs to whatever this connection did
         * last - a failed 'ls' three commands ago dresses this refusal
         * as "(Error 205: Object not found)", which the client prints
         * and the operator believes. Measured on the A1200. */
        return send_perr(fd,
            "ambiguous - several tasks match; use the 0x address from ps");
    if (hit == FindTask(NULL))
        return send_perr(fd, "that is wasabid itself - use restart or reboot");

    {
        struct Task *t;
        Disable();
        for (t = (struct Task *)SysBase->TaskReady.lh_Head;
             t->tc_Node.ln_Succ; t = (struct Task *)t->tc_Node.ln_Succ)
            if (t == hit) alive = TRUE;
        for (t = (struct Task *)SysBase->TaskWait.lh_Head;
             t->tc_Node.ln_Succ; t = (struct Task *)t->tc_Node.ln_Succ)
            if (t == hit) alive = TRUE;
        if (alive) {
            if (flags & 1) {
                /* RemTask'ing a slot's Shell leaves its runner waiting
                 * forever inside SystemTags: let go of that slot too. */
                LONG j;
                for (j = 0; j < MAX_JOBS; j++)
                    if (g_jobs[j].active && g_jobs[j].out &&
                        hit->tc_Node.ln_Type == NT_PROCESS &&
                        ((struct Process *)hit)->pr_COS == g_jobs[j].out)
                        orphan = &g_jobs[j];
                RemTask(hit);
            } else
                Signal(hit, SIGBREAKF_CTRL_C);
        }
        Enable();
    }
    if (orphan && !orphan->abandoned)
        job_abandon(orphan, "the command's process was removed with "
                            "'wasabi kill --force'");
    if (!alive)
        return send_perr(fd, "that task is already gone");
    return send_frame(fd, T_OK, NULL, 0);
}

/* --- commands ------------------------------------------------------ */

/*
 * Volume sizes, in megabytes and without touching a float.
 *
 * NumBlocks * BytesPerBlock overflows a LONG on anything past 4 GB - a
 * 58 GB drive at 512-byte blocks is 58e9 - so divide first: every real
 * block size (512, 1024, 2048, 4096) divides a megabyte exactly.
 */
static void vol_megabytes(struct InfoData *id, ULONG *total, ULONG *freemb)
{
    ULONG bpb = (ULONG)id->id_BytesPerBlock;
    ULONG per_mb;

    if (!bpb) bpb = 512;
    per_mb = 1048576UL / bpb;
    if (!per_mb) per_mb = 1;             /* absurd block size; do not divide by 0 */
    *total  = (ULONG)id->id_NumBlocks / per_mb;
    *freemb = (ULONG)(id->id_NumBlocks - id->id_NumBlocksUsed) / per_mb;
}

/*
 * Names are collected under the DOS list lock and everything else is
 * done after releasing it: Lock() itself wants the DOS list, and taking
 * it twice is how a machine stops responding.
 */
static BOOL info_volumes(int fd)
{
    char names[16][40];
    LONG count = 0, i;
    struct DosList *dl;
    char line[160];

    dl = LockDosList(LDF_VOLUMES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_VOLUMES | LDF_READ)) && count < 16) {
        UBYTE *b = (UBYTE *)BADDR(dl->dol_Name);
        LONG len = b ? b[0] : 0;
        if (len > 38) len = 38;
        memcpy(names[count], b + 1, len);
        names[count][len] = '\0';
        count++;
    }
    UnLockDosList(LDF_VOLUMES | LDF_READ);

    for (i = 0; i < count; i++) {
        struct InfoData id;
        char path[44];
        BPTR lock;
        ULONG total, freemb, pct = 0;
        LONG n;

        sprintf(path, "%s:", names[i]);
        lock = Lock(path, ACCESS_READ);  /* pr_WindowPtr is -1: no requester */
        if (!lock)
            continue;
        if (Info(lock, &id)) {
            vol_megabytes(&id, &total, &freemb);
            if (id.id_NumBlocks >= 100)
                pct = (ULONG)id.id_NumBlocksUsed /
                      ((ULONG)id.id_NumBlocks / 100);
            n = sprintf(line, "  %-14s %6lu MB total %6lu MB free  %3lu%% used%s\n",
                        path, (unsigned long)total, (unsigned long)freemb,
                        (unsigned long)pct,
                        id.id_DiskState == ID_WRITE_PROTECTED
                            ? "  (read-only)" : "");
            if (!send_frame(fd, T_DATA, line, n)) {
                UnLock(lock);
                return FALSE;
            }
        }
        UnLock(lock);
    }
    return TRUE;
}

static BOOL cmd_info(int fd)
{
    char text[512];
    LONG n = sprintf(text,
        "%s, protocol v%d\n"
        "exec.library %ld.%ld\n"
        "chip free %ld KB, fast free %ld KB\n",
        VERSION_STR, PROTO_VERSION,
        (long)SysBase->LibNode.lib_Version, (long)SysBase->LibNode.lib_Revision,
        (long)(AvailMem(MEMF_CHIP) >> 10), (long)(AvailMem(MEMF_FAST) >> 10));
    if (!send_frame(fd, T_DATA, text, n))
        return FALSE;
    n = patches_report(text, sizeof(text), g_dbg_client >= 0);
    n += sprintf(text + n, "volumes:\n");
    if (!send_frame(fd, T_DATA, text, n))
        return FALSE;
    if (!info_volumes(fd))
        return FALSE;
    return send_frame(fd, T_END, NULL, 0);
}

/* With no path, list the mounted volumes rather than failing. */
static BOOL ls_volumes(int fd)
{
    struct DosList *dl;
    char line[256];

    dl = LockDosList(LDF_VOLUMES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_VOLUMES | LDF_READ))) {
        UBYTE *bname = (UBYTE *)BADDR(dl->dol_Name);
        LONG len = bname ? bname[0] : 0;
        LONG n;
        char name[64];
        if (len > 60) len = 60;
        memcpy(name, bname + 1, len);
        name[len] = '\0';
        n = sprintf(line, "d 0 0 0 0 0 %s:\n", name);
        if (!send_frame(fd, T_DATA, line, n)) {
            UnLockDosList(LDF_VOLUMES | LDF_READ);
            return FALSE;
        }
    }
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    return send_frame(fd, T_END, NULL, 0);
}

static BOOL cmd_ls(int fd, const char *path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    char line[512];

    if (!path[0])
        return ls_volumes(fd);

    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock)
        return send_err(fd, "cannot lock that path");

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        return send_perr(fd, "out of memory");
    }
    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        return send_err(fd, "Examine failed");
    }
    while (ExNext(lock, fib)) {
        /* A positive type is a drawer - except a soft link (ST_SOFTLINK,
         * 3), which may point at a file: RAM:Disk.info is one, and
         * listing it as a drawer made it unopenable. */
        LONG n = sprintf(line, "%c %lu %ld %ld %ld %ld %s\n",
                         fib->fib_DirEntryType > 0 &&
                         fib->fib_DirEntryType != ST_SOFTLINK ? 'd' : 'f',
                         /* Unsigned on purpose: OS 3.x hands back a signed
                          * 32-bit size, so anything past 2 GB arrives
                          * negative. A file cannot be -1 bytes long, and
                          * reading it unsigned is right up to 4 GB - which
                          * is also the most this protocol can carry. */
                         (unsigned long)(ULONG)fib->fib_Size,
                         (long)fib->fib_Protection,
                         (long)fib->fib_Date.ds_Days,
                         (long)fib->fib_Date.ds_Minute,
                         (long)fib->fib_Date.ds_Tick,
                         fib->fib_FileName);
        if (!send_frame(fd, T_DATA, line, n)) {
            FreeDosObject(DOS_FIB, fib);
            UnLock(lock);
            return FALSE;
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return send_frame(fd, T_END, NULL, 0);
}

/* GET: open, and let pump_gets() send it a chunk per main-loop turn. */
static BOOL cmd_get(int cl, int fd, const char *path)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);

    if (!fh)
        return send_err(fd, "cannot open for reading");
    g_clients[cl].xfer = XF_GET;
    g_clients[cl].xfer_fh = fh;
    return TRUE;
}

/*
 * Is this path the binary we are running from? Compared with SameLock(),
 * so C:wasabid, DH0:C/wasabid and any assign that leads to the same file
 * are all recognised as one - a string compare would miss every alias.
 */
static BOOL is_self_file(const char *path)
{
    char self[128];
    BPTR a, b;
    LONG same = LOCK_DIFFERENT;

    if (!GetProgramName(self, sizeof(self)) || !self[0])
        return FALSE;
    a = Lock((STRPTR)path, ACCESS_READ);
    if (!a)
        return FALSE;                    /* nothing there yet - not us */
    b = Lock(self, ACCESS_READ);
    if (b) {
        same = SameLock(a, b);
        UnLock(b);
    }
    UnLock(a);
    return same == LOCK_SAME;
}

/*
 * Write to a sibling temp name and rename over the target at the end, so
 * an interrupted upload never leaves a half-written binary where a
 * working one used to be.
 *
 * One path is off limits: the binary this daemon is running from. A
 * plain put there would let any file at all - a truncated upload, or
 * simply the wrong one - become the daemon, and the next restart would
 * take the machine off the network with no way back but physical
 * access. That path has exactly one route, T_INSTALL, and only through
 * the verification `wasabi update` does first.
 */
/*
 * PUT: open the temporary file and return; the DATA frames that follow
 * are taken one per main-loop turn by put_frame(), between everybody
 * else's requests, and END renames the file into place - so a
 * half-written upload never stands where the old file was.
 */
static BOOL cmd_put(int cl, int fd, ULONG size, ULONG prot, const char *path)
{
    struct Client *c = &g_clients[cl];
    BPTR fh;

    (void)fd;
    c->xfer_size = size;
    c->xfer_got = 0;
    c->xfer_prot = prot;
    if (is_self_file(path)) {
        /* The client is already sending the body; take it and drop it,
         * or those DATA frames get read back as commands. */
        c->xfer = XF_SINK;
        c->xfer_why = "that is the running daemon - use 'wasabi update', "
                      "which verifies the binary before it commits";
        return TRUE;
    }
    sprintf(c->xfer_tmp, "%.280s.wasabi-tmp", path);
    copystr(c->xfer_path, sizeof(c->xfer_path), path);
    fh = Open(c->xfer_tmp, MODE_NEWFILE);
    if (!fh) {
        c->xfer = XF_SINK;
        c->xfer_why = "cannot create the temporary file";
        return TRUE;
    }
    c->xfer = XF_PUT;
    c->xfer_fh = fh;
    return TRUE;
}

/* A transfer's leftovers when its client goes or the daemon stops. */
static void xfer_abort(int cl)
{
    struct Client *c = &g_clients[cl];
    if (c->xfer_fh) {
        Close(c->xfer_fh);
        c->xfer_fh = 0;
        if (c->xfer == XF_PUT)
            DeleteFile(c->xfer_tmp);
    }
    c->xfer = XF_NONE;
}

/* One frame of a PUT in progress. FALSE: drop the client. */
static BOOL put_frame(int cl, UBYTE tag, UBYTE *p, LONG len)
{
    struct Client *c = &g_clients[cl];
    int fd = c->fd;

    if (tag == T_DATA) {
        if (c->xfer == XF_PUT && Write(c->xfer_fh, p, len) != len) {
            Close(c->xfer_fh);
            c->xfer_fh = 0;
            DeleteFile(c->xfer_tmp);
            c->xfer = XF_SINK;           /* take the rest, then say so */
            c->xfer_why = "write failed - disk full?";
        }
        c->xfer_got += (ULONG)len;
        return TRUE;
    }
    if (tag != T_END) {                  /* a command inside a body */
        xfer_abort(cl);
        return FALSE;
    }
    if (c->xfer == XF_SINK) {
        c->xfer = XF_NONE;
        return send_perr(fd, c->xfer_why);
    }
    Close(c->xfer_fh);
    c->xfer_fh = 0;
    c->xfer = XF_NONE;
    if (c->xfer_got != c->xfer_size) {
        DeleteFile(c->xfer_tmp);
        return send_perr(fd, "size mismatch");
    }
    DeleteFile(c->xfer_path);            /* Rename won't clobber */
    if (!Rename(c->xfer_tmp, (STRPTR)c->xfer_path)) {
        DeleteFile(c->xfer_tmp);
        return send_err(fd, "rename into place failed");
    }
    if (c->xfer_prot != 0xFFFFFFFFUL)
        SetProtection((STRPTR)c->xfer_path, (LONG)c->xfer_prot);
    return send_frame(fd, T_OK, NULL, 0);
}

static BOOL gets_pending(void)
{
    LONG i;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].fd >= 0 && g_clients[i].xfer == XF_GET)
            return TRUE;
    return FALSE;
}

/* One chunk of every GET in progress; END and close when it is all out. */
static void pump_gets(UBYTE *buf)
{
    LONG i, n;
    for (i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &g_clients[i];
        if (c->fd < 0 || c->xfer != XF_GET)
            continue;
        n = Read(c->xfer_fh, buf, MAX_PAYLOAD);
        if (n > 0) {
            if (!send_frame(c->fd, T_DATA, buf, n))
                drop((int)i);
            continue;
        }
        Close(c->xfer_fh);
        c->xfer_fh = 0;
        c->xfer = XF_NONE;
        if (n < 0 ? !send_err(c->fd, "read failed")
                  : !send_frame(c->fd, T_END, NULL, 0))
            drop((int)i);
    }
}


/* --- RUN, detached: start it, answer at once ----------------------- */

/*
 * A program with a window never ends by itself, and the one run slot
 * waits for its command to end - so starting a GUI program used to need
 * `Run >NIL:` typed in front. Detached, the command is handed to a new
 * Shell with SYS_Asynch, which closes its input and output when it
 * ends; it takes no run slot, and its output goes to a file in T: whose
 * name the client is told, to read later with GET.
 *
 * Separate stderr (RUN flag bit 0) stays unhonoured, for a reason: the
 * error channel for SystemTags, SYS_Error, is new in V50 (OS 4) - not
 * in 3.2's V47 - and 3.x programs print their errors to Output()
 * anyway.
 */
static BOOL cmd_run_detached(int fd, const char *cmd)
{
    static LONG serial;
    char name[48], note[100];
    BPTR in, out;
    LONG rc, n;
    UBYTE ex[8];

    in = Open("NIL:", MODE_OLDFILE);
    /* The numbering restarts with the daemon, and a program started by
     * the last one may still hold its file open: take the next free. */
    out = 0;
    for (n = 0; !out && n < 50; n++) {
        sprintf(name, "T:wasabi-bg-%ld", (long)++serial);
        out = Open(name, MODE_NEWFILE);
    }
    if (!in || !out) {
        if (in) Close(in);
        if (out) Close(out);
        return send_err(fd, "cannot open the output file in T:");
    }
    rc = SystemTags((STRPTR)cmd,
                    SYS_Input, (ULONG)in,
                    SYS_Output, (ULONG)out,
                    SYS_Asynch, TRUE,
                    SYS_UserShell, TRUE,
                    TAG_DONE);
    if (rc == -1) {                      /* not started: the files are ours */
        Close(in);
        Close(out);
        DeleteFile(name);
        return send_err(fd, "could not start the command");
    }
    n = sprintf(note, "[started in the background; its output goes to %s]\n",
                name);
    if (!send_frame(fd, T_STDERR, note, n))
        return FALSE;
    put_be32(ex, 0);
    put_be32(ex + 4, 0);
    return send_frame(fd, T_EXIT, ex, 8);
}

/* --- CLIP: the Amiga's clipboard, as text ------------------------- */

/*
 * Unit 0 (PRIMARY_CLIP), the one every program uses; text as IFF FORM
 * FTXT with one CHRS chunk, LF line ends - the format the whole console
 * family shares (Knowledge/amiga/clipboard-and-selection.md). Opened
 * per command, not kept: nothing here should hold a device open
 * between commands it may never be sent.
 *
 * op 0, read: DATA with the text (Latin-1), then END; nothing on the
 * clipboard, or no text on it, is an empty answer. The read is always
 * run dry - a clip left half-read stays held and blocks the next writer.
 * op 1, write: the rest of the payload is the text; CMD_UPDATE after
 * the write publishes it.
 */
#define CLIP_KEEP (60 * 1024)

static ULONG get_be32u(const UBYTE *b)
{
    return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) | ((ULONG)b[2] << 8) | b[3];
}

static BOOL cmd_clip(int fd, ULONG op, const UBYTE *text, LONG tlen)
{
    struct MsgPort *mp;
    struct IOClipReq *io;
    UBYTE *buf;
    static UBYTE scratch[256];
    LONG got = 0, size;
    BOOL ok = TRUE;

    if (op > 1)
        return send_perr(fd, "bad CLIP op");
    if (op == 1 && tlen > CLIP_KEEP)
        return send_perr(fd, "that is too much text for one clip");
    size = op == 1 ? 20 + tlen + 1 : CLIP_KEEP + 64;
    buf = AllocMem(size, MEMF_ANY | MEMF_CLEAR);
    mp = CreateMsgPort();
    io = mp ? (struct IOClipReq *)CreateIORequest(mp, sizeof(*io)) : NULL;
    if (!buf || !io || OpenDevice("clipboard.device", PRIMARY_CLIP,
                                  (struct IORequest *)io, 0) != 0) {
        if (io) DeleteIORequest((struct IORequest *)io);
        if (mp) DeleteMsgPort(mp);
        if (buf) FreeMem(buf, size);
        return send_err(fd, "cannot open clipboard.device");
    }

    if (op == 1) {
        LONG pad = tlen & 1;
        put_be32(buf, 0x464F524DUL);               /* FORM */
        put_be32(buf + 4, 12 + tlen + pad);
        put_be32(buf + 8, 0x46545854UL);           /* FTXT */
        put_be32(buf + 12, 0x43485253UL);          /* CHRS */
        put_be32(buf + 16, tlen);                  /* not padded */
        memcpy(buf + 20, text, tlen);
        io->io_Command = CMD_WRITE;
        io->io_Data = (STRPTR)buf;
        io->io_Length = 20 + tlen + pad;
        io->io_Offset = 0;
        io->io_ClipID = 0;
        DoIO((struct IORequest *)io);
        ok = io->io_Error == 0;
        if (ok) {
            io->io_Command = CMD_UPDATE;           /* publishes it */
            DoIO((struct IORequest *)io);
            ok = io->io_Error == 0;
        }
    } else {
        io->io_Command = CMD_READ;
        io->io_Data = (STRPTR)buf;
        io->io_Length = CLIP_KEEP + 64;
        io->io_Offset = 0;
        io->io_ClipID = 0;
        DoIO((struct IORequest *)io);
        got = io->io_Error ? 0 : (LONG)io->io_Actual;
        do {                                       /* run it dry */
            io->io_Command = CMD_READ;
            io->io_Data = (STRPTR)scratch;
            io->io_Length = sizeof(scratch);
            DoIO((struct IORequest *)io);
        } while (io->io_Error == 0 && io->io_Actual > 0);
    }
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(mp);

    if (op == 1) {
        FreeMem(buf, size);
        return ok ? send_frame(fd, T_OK, NULL, 0)
                  : send_err(fd, "the clipboard refused the write");
    }
    /* Find the text: FORM....FTXT, then walk the chunks for CHRS. */
    if (got >= 12 && get_be32u(buf) == 0x464F524DUL &&
        get_be32u(buf + 8) == 0x46545854UL) {
        LONG o = 12;
        while (o + 8 <= got) {
            ULONG id = get_be32u(buf + o), sz = get_be32u(buf + o + 4);
            if (id == 0x43485253UL) {
                LONG n = (LONG)sz, at = o + 8;
                if (n > got - at)
                    n = got - at;                  /* trust bytes, not sizes */
                for (o = 0; ok && o < n; o += MAX_PAYLOAD)
                    ok = send_frame(fd, T_DATA, buf + at + o,
                                    n - o > MAX_PAYLOAD ? MAX_PAYLOAD : n - o);
                break;
            }
            /* A corrupt clip may declare any size: past the bytes in hand,
             * stop - the sum used to wrap negative and read before buf. */
            if (sz > (ULONG)(got - o - 8))
                break;
            o += 8 + sz + (sz & 1);                /* chunks are even */
        }
    }
    FreeMem(buf, size);
    return ok ? send_frame(fd, T_END, NULL, 0) : FALSE;
}

/* --- remote mouse: move the pointer, click, doubleclick ------------ */

/*
 * Written into the input stream with IND_WRITEEVENT, so Intuition
 * treats them exactly like real mouse input: the pointer moves, the
 * window under it gets the click, double-click timing comes from the
 * event timestamps. IECLASS_POINTERPOS carries absolute screen
 * coordinates - the same pixel space GRAB delivers, which is the
 * point: grab, look, click.
 *
 * The x/y value -32768 means "no position": click where the pointer
 * already is.
 *
 * DOWN and UP are separate verbs because the press-hold-move-release
 * gesture is how Intuition menus (and every drag) work: right button
 * down, glide over the menu bar - grab shows the open pane, menus
 * render into the screen bitmap like everything else - move to the
 * item, release. The daemon remembers which buttons are held across
 * connections so the moves in between carry the held buttons'
 * qualifiers, which is what Intuition's menu tracking watches. A
 * hold left dangling by a dead client is cleared by the next real
 * mouse press, or an UP with the same button.
 */
#define INPUT_NOPOS (-32768)

static UWORD g_mouse_held;      /* qualifier bits of held buttons */

static BOOL write_input_event(struct IOStdReq *io, struct InputEvent *ie)
{
    ULONG s, m;

    CurrentTime(&s, &m);
    ie->ie_TimeStamp.tv_secs = s;
    ie->ie_TimeStamp.tv_micro = m;
    ie->ie_NextEvent = NULL;
    io->io_Command = IND_WRITEEVENT;
    io->io_Data = ie;
    io->io_Length = sizeof(struct InputEvent);
    return DoIO((struct IORequest *)io) == 0;
}

/* input.device on a fresh port, or NULL. One per command: the daemon
 * holds nothing open between commands it may never be sent. */
static struct IOStdReq *open_input(void)
{
    struct MsgPort *mp = CreateMsgPort();
    struct IOStdReq *io = mp ? (struct IOStdReq *)
        CreateExtIO(mp, sizeof(struct IOStdReq)) : NULL;

    if (io && OpenDevice("input.device", 0,
                         (struct IORequest *)io, 0) == 0)
        return io;
    if (io) DeleteExtIO((struct IORequest *)io);
    if (mp) DeleteMsgPort(mp);
    return NULL;
}

static void close_input(struct IOStdReq *io)
{
    struct MsgPort *mp = io->io_Message.mn_ReplyPort;

    CloseDevice((struct IORequest *)io);
    DeleteExtIO((struct IORequest *)io);
    DeleteMsgPort(mp);
}

/* The mouse events for one MOUSE command, on an open input.device.
 * The caller has checked the parameters. */
static BOOL mouse_events(struct IOStdReq *io, ULONG action, ULONG button,
                         ULONG count, WORD x, WORD y)
{
    struct InputEvent ie;
    BOOL ok = TRUE;
    static const UWORD codes[3] = {
        IECODE_LBUTTON, IECODE_RBUTTON, IECODE_MBUTTON
    };
    static const UWORD quals[3] = {
        IEQUALIFIER_LEFTBUTTON, IEQUALIFIER_RBUTTON, IEQUALIFIER_MIDBUTTON
    };
    ULONG i;

    if (x != INPUT_NOPOS) {
        memset(&ie, 0, sizeof(ie));
        ie.ie_Class = IECLASS_POINTERPOS;
        ie.ie_Code = IECODE_NOBUTTON;
        ie.ie_Qualifier = g_mouse_held;  /* a held drag stays a drag */
        ie.ie_X = x;
        ie.ie_Y = y;
        ok = write_input_event(io, &ie);
    }

    for (i = 0; ok && action == 1 && i < count; i++) {
        memset(&ie, 0, sizeof(ie));
        ie.ie_Class = IECLASS_RAWMOUSE;
        ie.ie_Code = codes[button];
        ie.ie_Qualifier = quals[button] | g_mouse_held;
        ok = write_input_event(io, &ie);
        if (ok) {
            memset(&ie, 0, sizeof(ie));
            ie.ie_Class = IECLASS_RAWMOUSE;
            ie.ie_Code = codes[button] | IECODE_UP_PREFIX;
            ie.ie_Qualifier = g_mouse_held;
            ok = write_input_event(io, &ie);
        }
    }

    if (ok && action == 2) {            /* press and hold */
        memset(&ie, 0, sizeof(ie));
        ie.ie_Class = IECLASS_RAWMOUSE;
        ie.ie_Code = codes[button];
        ie.ie_Qualifier = quals[button] | g_mouse_held;
        ok = write_input_event(io, &ie);
        if (ok)
            g_mouse_held |= quals[button];
    }

    if (ok && action == 3) {            /* release */
        g_mouse_held &= ~quals[button];
        memset(&ie, 0, sizeof(ie));
        ie.ie_Class = IECLASS_RAWMOUSE;
        ie.ie_Code = codes[button] | IECODE_UP_PREFIX;
        ie.ie_Qualifier = g_mouse_held;
        ok = write_input_event(io, &ie);
    }
    return ok;
}

static const char *do_mouse(ULONG action, ULONG button, ULONG count,
                            WORD x, WORD y)
{
    struct IOStdReq *io;
    BOOL ok;

    if (button > 2 || count > 3 || action > 3)
        return "bad INPUT parameters";

    if (!(io = open_input()))
        return "cannot open input.device";
    ok = mouse_events(io, action, button, count, x, y);
    close_input(io);

    return ok ? NULL : "input event write failed";
}

static BOOL cmd_input(int fd, ULONG action, ULONG button, ULONG count,
                      WORD x, WORD y)
{
    const char *why = do_mouse(action, button, count, x, y);
    return why ? send_perr(fd, why) : send_frame(fd, T_OK, NULL, 0);
}

/* --- remote keyboard: key positions, and text ---------------------- */

/*
 * Two ways in, both IECLASS_RAWKEY events through IND_WRITEEVENT, the
 * same road as the mouse:
 *
 * KEY_RAW sends key *positions* - raw key codes with their qualifiers,
 * exactly what the keyboard itself produces. The Amiga's own keymap
 * then decides which character that is, so a Swedish keymap behaves
 * as it does at the real keyboard. This is the live view's road.
 *
 * KEY_TEXT sends *characters* (Latin-1). keymap.library's MapANSI()
 * turns each into the key presses the current default keymap needs
 * to produce it - including the dead-key prefix for an accented
 * letter the keymap only reaches that way, which goes out the way the
 * autodoc describes: one event, the earlier keys in ie_Prev1Down/
 * ie_Prev2Down. So `wasabi key type` gets the text right whatever the
 * layout, and needs no key map on the PC side.
 *
 * Held mouse buttons stay held: their qualifier bits are ORed in, so a
 * shift-drag is possible.
 */
#define KEY_RAW   0
#define KEY_TEXT  1
#define KEY_MAX_TEXT 2048

static BOOL key_event(struct IOStdReq *io, UWORD code, UWORD qual,
                      const UBYTE *prev, LONG nprev)
{
    struct InputEvent ie;

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = code;
    ie.ie_Qualifier = qual | g_mouse_held;
    if (nprev == 2) {                   /* two dead keys, then this one */
        ie.ie_Prev2DownCode = prev[0];
        ie.ie_Prev2DownQual = prev[1];
        ie.ie_Prev1DownCode = prev[2];
        ie.ie_Prev1DownQual = prev[3];
    } else if (nprev == 1) {
        ie.ie_Prev1DownCode = prev[0];
        ie.ie_Prev1DownQual = prev[1];
    }
    return write_input_event(io, &ie);
}

/* Can the current keymap make character c? (keymap.library is open.) */
static BOOL can_type(UBYTE c)
{
    UBYTE pairs[6];
    return MapANSI((STRPTR)&c, 1, (STRPTR)pairs, 3, NULL) > 0;
}

static const char *cannot_type(UBYTE c)
{
    static char msg[64];
    sprintf(msg, "the keymap cannot type character %u (0x%02x)",
            (unsigned)c, (unsigned)c);
    return msg;
}

/* Press and release the keys that make character c, dead keys and all. */
static BOOL type_char(struct IOStdReq *io, UBYTE c)
{
    UBYTE pairs[6];                     /* up to two dead keys + the key */
    UWORD code, qual;
    LONG n = MapANSI((STRPTR)&c, 1, (STRPTR)pairs, 3, NULL);

    if (n <= 0 || n > 3)
        return TRUE;                    /* checked before anything went */
    code = pairs[(n - 1) * 2];
    qual = pairs[(n - 1) * 2 + 1];
    if (!key_event(io, code, qual, pairs, n - 1))
        return FALSE;
    return key_event(io, code | IECODE_UP_PREFIX, qual, NULL, 0);
}

static const char *do_key(ULONG mode, const UBYTE *p, LONG len)
{
    struct IOStdReq *io;
    LONG i;
    BOOL ok = TRUE;

    if (mode == KEY_RAW && (len % 4) != 0)
        return "bad KEY event list";
    if (mode == KEY_TEXT && len > KEY_MAX_TEXT)
        return "KEY text too long";
    if (mode > KEY_TEXT)
        return "bad KEY mode";

    if (mode == KEY_TEXT && !g_keymap)
        g_keymap = OpenLibrary("keymap.library", 37);
    if (mode == KEY_TEXT && !g_keymap)
        return "cannot open keymap.library";

    /* Map everything before the first event goes out: a character the
     * keymap cannot make must fail the command, not leave half a line
     * typed into somebody's window. */
    for (i = 0; mode == KEY_TEXT && i < len; i++)
        if (!can_type(p[i]))
            return cannot_type(p[i]);

    if (!(io = open_input()))
        return "cannot open input.device";

    if (mode == KEY_RAW) {
        for (i = 0; ok && i < len; i += 4)
            ok = key_event(io, (UWORD)((p[i] << 8) | p[i + 1]),
                           (UWORD)((p[i + 2] << 8) | p[i + 3]), NULL, 0);
    } else {
        for (i = 0; ok && i < len; i++)
            ok = type_char(io, p[i]);
    }

    close_input(io);

    return ok ? NULL : "input event write failed";
}

static BOOL cmd_key(int fd, ULONG mode, const UBYTE *p, LONG len)
{
    const char *why = do_key(mode, p, len);
    return why ? send_perr(fd, why) : send_frame(fd, T_OK, NULL, 0);
}

/* --- INPUTS: a batch of key and mouse events, timed by the Amiga --- */

/*
 * Separate KEY and MOUSE commands are a network round trip each, so
 * the gaps between them are whatever the PC and the network make them.
 * INPUTS sends the whole gesture at once - a drag, a chord, a burst of
 * typing - and the daemon plays it with Delay() between events, so the
 * gaps are the Amiga's own ticks (1/50 s), the same every time.
 *
 * Payload: 12-byte records, big-endian:
 *   u8 kind, u8 button, u16 wait, u16 a, u16 b, i16 x, i16 y
 * kind 0  raw key: a = code (bit 7 = up), b = qualifier
 * kind 1  character (Latin-1) in a, typed through the keymap as KEY
 *         text is: the keys that make it, pressed and released
 * kind 2  mouse: a = action (0 move, 1 click, 2 down, 3 up),
 *         b = click count, button 0 left 1 right 2 middle, x/y as MOUSE
 * wait    ticks to Delay() after this event, before the next.
 *
 * Every record is checked before the first event goes out, as KEY
 * text is: a bad one must fail the batch, not leave half a drag done.
 * The daemon does nothing else while it plays, so the waits are capped
 * (INPUTS_MAX_TICKS) - a batch is a gesture, not a script.
 */
#define INPUTS_REC        12
#define INPUTS_MAX        1024
#define INPUTS_MAX_TICKS  250           /* 5 s */

static BOOL cmd_inputs(int fd, const UBYTE *p, LONG len)
{
    struct IOStdReq *io;
    LONG n = len / INPUTS_REC, i;
    ULONG ticks = 0;
    BOOL ok = TRUE, chars = FALSE;
    static char msg[80];

    if (len <= 0 || len % INPUTS_REC || n > INPUTS_MAX)
        return send_perr(fd, "bad INPUTS list");
    for (i = 0; i < n; i++) {
        const UBYTE *r = p + i * INPUTS_REC;
        UWORD a = (UWORD)((r[4] << 8) | r[5]);
        UWORD b = (UWORD)((r[6] << 8) | r[7]);
        ticks += (ULONG)((r[2] << 8) | r[3]);
        if (r[0] == 1)
            chars = TRUE;
        if (r[0] > 2 || (r[0] == 1 && a > 255) ||
            (r[0] == 2 && (r[1] > 2 || a > 3 || b > 3))) {
            sprintf(msg, "bad INPUTS event %ld", (long)i + 1);
            return send_perr(fd, msg);
        }
    }
    if (ticks > INPUTS_MAX_TICKS)
        return send_perr(fd, "INPUTS waits add up to more than 5 s");
    if (chars && !g_keymap)
        g_keymap = OpenLibrary("keymap.library", 37);
    if (chars && !g_keymap)
        return send_perr(fd, "cannot open keymap.library");
    for (i = 0; chars && i < n; i++) {
        const UBYTE *r = p + i * INPUTS_REC;
        if (r[0] == 1 && !can_type(r[5]))
            return send_perr(fd, cannot_type(r[5]));
    }

    if (!(io = open_input()))
        return send_perr(fd, "cannot open input.device");
    for (i = 0; ok && i < n; i++) {
        const UBYTE *r = p + i * INPUTS_REC;
        UWORD wait = (UWORD)((r[2] << 8) | r[3]);
        UWORD a = (UWORD)((r[4] << 8) | r[5]);
        UWORD b = (UWORD)((r[6] << 8) | r[7]);
        WORD x = (WORD)((r[8] << 8) | r[9]);
        WORD y = (WORD)((r[10] << 8) | r[11]);
        if (r[0] == 0)
            ok = key_event(io, a, b, NULL, 0);
        else if (r[0] == 1)
            ok = type_char(io, (UBYTE)a);
        else
            ok = mouse_events(io, a, r[1], b, x, y);
        if (ok && wait)
            Delay(wait);
    }
    close_input(io);
    if (!ok) {
        sprintf(msg, "input event write failed at event %ld", (long)i);
        return send_perr(fd, msg);
    }
    return send_frame(fd, T_OK, NULL, 0);
}

/* --- every screen and window, as text ------------------------------ */

/*
 * The UI as facts rather than pixels: click a window's close gadget
 * from its position, not by guessing where it is in a picture.
 *
 * One line per screen, front first, each followed by its windows,
 * front first, fields separated by tabs (titles have spaces):
 *
 *   S <addr> <left> <top> <width> <height> <depth> <title>
 *   W <addr> <left> <top> <width> <height>
 *     <borderleft> <bordertop> <borderright> <borderbottom>
 *     <flags> <task> <title>
 *
 * Window positions are relative to their screen - the same pixel space
 * GRAB and MOUSE use. flags: 'a' the active window, 'b' a backdrop,
 * '-' neither. <task> is who reads the window's IDCMP port: the
 * command name for a CLI program, the task name otherwise; '-' when
 * the window has no port.
 *
 * The walk runs under LockIBase(), into a buffer; the lines go to the
 * socket only after the lock is dropped. A slow network must never
 * hold Intuition still - that would freeze the very screen being
 * looked at.
 */
#define WIN_BUF 16384

static void win_task_name(struct Window *w, char *out, LONG outsz)
{
    struct Task *t;

    out[0] = '\0';
    if (!w->UserPort || !(t = (struct Task *)w->UserPort->mp_SigTask))
        return;
    if (t->tc_Node.ln_Type == NT_PROCESS) {
        struct CommandLineInterface *cli = (struct CommandLineInterface *)
            BADDR(((struct Process *)t)->pr_CLI);
        UBYTE *b = cli ? (UBYTE *)BADDR(cli->cli_CommandName) : NULL;
        if (b && b[0]) {                /* a BSTR: length, then bytes */
            LONG bn = b[0] < outsz - 1 ? b[0] : outsz - 1;
            memcpy(out, b + 1, bn);
            out[bn] = '\0';
            return;
        }
    }
    copystr(out, outsz, t->tc_Node.ln_Name);
}

/* Tabs and newlines in a title would break the line format. */
static void win_clean(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

/*
 * OS 2.x has none of graphics.library 39's bitmap and colour calls;
 * calling one there jumps through a missing vector and takes the
 * machine down. These stand in for them.
 */
static BOOL gfx39(void)
{
    return GfxBase->LibNode.lib_Version >= 39;
}

static LONG bm_depth(struct BitMap *bm)
{
    return gfx39() ? (LONG)GetBitMapAttr(bm, BMA_DEPTH) : (LONG)bm->Depth;
}

/* GetRGB32's table (three 32-bit components a colour), from the 12-bit
 * colours GetRGB4 gives on OS 2.x. */
static void get_rgb32(struct ColorMap *cm, ULONG n, ULONG *table)
{
    ULONG i;
    if (gfx39()) {
        GetRGB32(cm, 0, n, table);
        return;
    }
    for (i = 0; i < n; i++) {
        ULONG c = (ULONG)GetRGB4(cm, (LONG)i);
        table[i * 3]     = ((c >> 8) & 15) * 0x11111111UL;
        table[i * 3 + 1] = ((c >> 4) & 15) * 0x11111111UL;
        table[i * 3 + 2] = (c & 15) * 0x11111111UL;
    }
}

static void scratch_free(struct BitMap *bm)
{
    LONG i;
    if (!bm)
        return;
    if (gfx39()) {
        FreeBitMap(bm);
        return;
    }
    for (i = 0; i < bm->Depth; i++)
        if (bm->Planes[i])
            FreeRaster(bm->Planes[i], (UWORD)(bm->BytesPerRow * 8), 1);
    FreeMem(bm, sizeof(*bm));
}

static struct BitMap *scratch_row(struct Screen *sc, LONG aligned);

static BOOL cmd_windows(int fd)
{
    struct Screen *sc;
    struct Window *w, *active;
    char *buf, *line;
    char task[48], title[120];
    LONG used = 0, n, i;
    BOOL ok = TRUE, full = FALSE;
    ULONG ib;

    buf = AllocMem(WIN_BUF, MEMF_ANY);
    if (!buf)
        return send_err(fd, "out of memory");
    line = buf;

    ib = LockIBase(0);
    active = IntuitionBase->ActiveWindow;
    for (sc = IntuitionBase->FirstScreen; sc && !full; sc = sc->NextScreen) {
        copystr(title, sizeof(title), sc->Title ? (char *)sc->Title : "");
        win_clean(title);
        if (used + 300 > WIN_BUF) { full = TRUE; break; }
        used += sprintf(buf + used, "S\t0x%08lx\t%ld\t%ld\t%ld\t%ld\t%ld\t%s\t%ld\t%ld\n",
                        (unsigned long)sc, (long)sc->LeftEdge,
                        (long)sc->TopEdge, (long)sc->Width,
                        (long)sc->Height,
                        (long)bm_depth(sc->RastPort.BitMap),
                        title, (long)sc->MouseX, (long)sc->MouseY);
        for (w = sc->FirstWindow; w; w = w->NextWindow) {
            char flags[3];
            LONG f = 0;
            if (used + 300 > WIN_BUF) { full = TRUE; break; }
            if (w == active) flags[f++] = 'a';
            if (w->Flags & WFLG_BACKDROP) flags[f++] = 'b';
            if (!f) flags[f++] = '-';
            flags[f] = '\0';
            win_task_name(w, task, sizeof(task));
            win_clean(task);
            copystr(title, sizeof(title), w->Title ? (char *)w->Title : "");
            win_clean(title);
            used += sprintf(buf + used,
                    "W\t0x%08lx\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld"
                    "\t%s\t%s\t%s\n",
                    (unsigned long)w, (long)w->LeftEdge, (long)w->TopEdge,
                    (long)w->Width, (long)w->Height,
                    (long)w->BorderLeft, (long)w->BorderTop,
                    (long)w->BorderRight, (long)w->BorderBottom,
                    flags, task[0] ? task : "-", title);
        }
    }
    UnlockIBase(ib);

    /* Whole lines per frame, so the client can split on newlines. */
    for (i = 0; ok && i < used; i += n) {
        n = used - i;
        if (n > 8192) {
            n = 8192;
            while (n > 0 && line[i + n - 1] != '\n')
                n--;
        }
        ok = send_frame(fd, T_DATA, line + i, n);
    }
    FreeMem(buf, WIN_BUF);
    if (!ok)
        return FALSE;
    if (full)
        return send_err(fd, "too many windows; the list is cut short");
    return send_frame(fd, T_END, NULL, 0);
}

/* --- the GUI as objects: gadgets and menus ------------------------- */

/*
 * UI: what `windows` says, plus each window's gadgets and menu strip,
 * so a client can click "the OK button" or pick "Project/Quit" by name
 * instead of by guessed pixels.
 *
 *   S and W lines exactly as WINDOWS sends them, then after each W:
 *   G <addr> <left> <top> <width> <height> <kind> <flags> <id>
 *     <class> <label> <value>
 *   M <code> <flags> <key> <path>
 *
 * G boxes are relative to the window's top-left, already resolved for
 * GFLG_RELRIGHT/RELBOTTOM/RELWIDTH/RELHEIGHT and for a GimmeZeroZero
 * window's inner layer. kind is close, depth, zoom, size, drag, sdepth,
 * sdrag, button, toggle, prop, string, integer, custom. flags: 'd'
 * disabled, 's' selected, '-' neither. class is a BOOPSI gadget's class
 * name ('-' otherwise). label is the gadget's own text, IntuiText runs
 * joined by spaces, the shortcut underscore removed. value: a string
 * gadget's contents, a prop's pot as 0-65535.
 *
 * M code is FULLMENUNUM (menu | item << 5 | sub << 11, 31 = none), what
 * a MENUPICK carries and what UIACT's menu pick wants back. flags: 'd'
 * disabled (the item or its menu), 'c' checkable, 'x' checked, 'b' a
 * bar (no text), '-' none. key is the Amiga-key shortcut or '-'.
 *
 * Same discipline as WINDOWS: walked under LockIBase() into a buffer,
 * sent after the lock is dropped. Nothing here calls into a gadget's
 * class - a dispatcher run from our task under the IBase lock could
 * wait on a layer its own task holds, and that is a frozen screen.
 */
#define UI_BUF 49152

#define GTYPMASK_SYS 0x00F0
static const char *ui_kind(struct Gadget *g)
{
    UWORD t = g->GadgetType;

    if (t & GTYP_SYSGADGET) {
        switch (t & GTYPMASK_SYS) {
        case GTYP_SIZING:    return "size";
        case GTYP_WDRAGGING: return "drag";
        case GTYP_SDRAGGING: return "sdrag";
        case GTYP_WDEPTH:    return "depth";
        case GTYP_SDEPTH:    return "sdepth";
        case GTYP_WZOOM:     return "zoom";
        case GTYP_CLOSE:     return "close";
        }
        return "system";
    }
    switch (t & GTYP_GTYPEMASK) {
    case GTYP_PROPGADGET:   return "prop";
    case GTYP_STRGADGET:
        return (g->Activation & GACT_LONGINT) ? "integer" : "string";
    case GTYP_CUSTOMGADGET: return "custom";
    }
    return (g->Activation & GACT_TOGGLESELECT) ? "toggle" : "button";
}

/* Append src to out (at *n, capacity sz) without tabs or newlines;
 * a gadget label also loses the underscore that marks its shortcut. */
static void ui_putx(char *out, LONG *n, LONG sz, const UBYTE *src,
                    BOOL label)
{
    for (; src && *src && *n < sz - 1; src++) {
        UBYTE c = *src;
        if (label && c == '_')
            continue;
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
        out[(*n)++] = c;
    }
    out[*n] = '\0';
}

#define ui_put(o, n, sz, src) ui_putx(o, n, sz, src, FALSE)

/* Memory we may read: RAM exec knows, or the ROM (where a ROM font's
 * name lives). Anything else might not be there at all. */
static BOOL ui_readable(const void *p)
{
    ULONG a = (ULONG)p;
    if (!a || (a & 1))
        return FALSE;
    if (a >= 0xF80000UL && a < 0x1000000UL)
        return TRUE;
    return TypeOfMem((APTR)p) != 0;
}

/* Text may start at any byte; structures and pointers to them may not. */
static BOOL ui_readable_text(const void *p)
{
    ULONG a = (ULONG)p;
    if (!a)
        return FALSE;
    if (a >= 0xF80000UL && a < 0x1000000UL)
        return TRUE;
    return TypeOfMem((APTR)p) != 0;
}

static BOOL ui_printable(const UBYTE *t)
{
    int k;
    if (!ui_readable_text(t))
        return FALSE;
    for (k = 0; k < 80 && t[k]; k++)
        if (t[k] < 32 || (t[k] > 126 && t[k] < 160))
            return FALSE;
    return k > 0 && k < 80;
}

/*
 * A GadTools button keeps its text out of GadgetText: GadTools draws it
 * from an IntuiText in its own memory just after the ExtGadget, and a
 * cycle gadget keeps its current choice the same way (checked on OS
 * 3.2's gadtools 47). That memory is private, so it is only believed if
 * it looks exactly like an IntuiText: a draw mode Intuition knows, a
 * font whose TextAttr names a ".font", and printable text. Read-only,
 * a bounded window, every pointer checked before it is followed.
 */
#define GTYP_GADTOOLS_FLAG 0x0100
static const UBYTE *ui_gt_text(struct Gadget *g)
{
    const UBYTE *base = (const UBYTE *)g + sizeof(struct ExtGadget);
    LONG off;

    if (!(g->GadgetType & GTYP_GADTOOLS_FLAG) || !(g->Flags & GFLG_EXTENDED))
        return NULL;
    for (off = 0; off <= 80; off += 2) {
        const struct IntuiText *it = (const struct IntuiText *)(base + off);
        const struct TextAttr *ta = it->ITextFont;
        const UBYTE *nm;
        int k;
        if (it->DrawMode > 7 || ((const UBYTE *)it)[3] != 0 || !ui_readable(ta))
            continue;
        nm = (const UBYTE *)ta->ta_Name;
        if (!ui_printable(nm))
            continue;
        for (k = 0; nm[k]; k++)
            ;
        if (k < 6 || strcmp((const char *)nm + k - 5, ".font") != 0)
            continue;
        if (ui_printable(it->IText))
            return it->IText;
    }
    return NULL;
}

static void ui_label(struct Gadget *g, char *out, LONG sz)
{
    LONG n = 0;

    out[0] = '\0';
    if (!g->GadgetText)
        return;
    switch (g->Flags & GFLG_LABELMASK) {
    case GFLG_LABELSTRING:
        ui_putx(out, &n, sz, (UBYTE *)g->GadgetText, TRUE);
        break;
    case GFLG_LABELITEXT: {
        struct IntuiText *it;
        int k = 0;
        for (it = g->GadgetText; it && k < 8; it = it->NextText, k++) {
            if (!it->IText)
                continue;
            /* GadTools underlines a shortcut letter by drawing it again
             * as a text of its own: "Name" then "N". Not a word. */
            if (n && it->IText[0] && !it->IText[1] &&
                strchr(out, it->IText[0]))
                continue;
            if (n && n < sz - 1)
                out[n++] = ' ';
            ui_putx(out, &n, sz, it->IText, TRUE);
        }
        break;
    }
    }                                   /* LABELIMAGE: a picture, no text */
}

/* One menu item's text: an IntuiText run, or nothing for an image. */
static void ui_item_text(struct MenuItem *mi, char *out, LONG *n, LONG sz)
{
    struct IntuiText *it;
    int k = 0;
    LONG start = *n;

    if (!(mi->Flags & ITEMTEXT) || !mi->ItemFill)
        return;
    for (it = (struct IntuiText *)mi->ItemFill; it && k < 4;
         it = it->NextText, k++) {
        if (!it->IText)
            continue;
        if (*n > start && *n < sz - 1)
            out[(*n)++] = ' ';
        ui_put(out, n, sz, it->IText);
    }
    /* A '/' inside a name would split the path; keep it readable. */
    for (; start < *n; start++)
        if (out[start] == '/')
            out[start] = '|';
}

/* One M line. parent is the item a sub-item hangs off, or NULL. */
static LONG ui_menu_line(char *buf, LONG used, struct Menu *m,
                         struct MenuItem *parent, struct MenuItem *x,
                         ULONG code, const char *path)
{
    char fl[6], key[2];
    int f = 0;

    if (!(m->Flags & MENUENABLED) || !(x->Flags & ITEMENABLED) ||
        (parent && !(parent->Flags & ITEMENABLED)))
        fl[f++] = 'd';
    if (x->Flags & CHECKIT) fl[f++] = 'c';
    /* CHECKED means nothing without CHECKIT (Intuition ignores it), and
     * some programs leave it set on plain items. */
    if ((x->Flags & CHECKIT) && (x->Flags & CHECKED)) fl[f++] = 'x';
    if (!(x->Flags & ITEMTEXT)) fl[f++] = 'b';
    if (!f) fl[f++] = '-';
    fl[f] = '\0';
    key[0] = ((x->Flags & COMMSEQ) && x->Command) ? x->Command : '-';
    key[1] = '\0';
    return used + sprintf(buf + used, "M\t%lu\t%s\t%s\t%s\n",
                          (unsigned long)code, fl, key, path);
}

static LONG ui_menus(struct Window *w, char *buf, LONG used, LONG cap,
                     BOOL *full)
{
    struct Menu *m;
    struct MenuItem *it, *sub;
    ULONG mn, in, sn;
    char path[200];
    LONG pm, pi, pn;

    for (m = w->MenuStrip, mn = 0; m && mn < 31; m = m->NextMenu, mn++) {
        pm = 0;
        path[0] = '\0';
        ui_put(path, &pm, sizeof(path) - 2, (UBYTE *)m->MenuName);
        for (it = m->FirstItem, in = 0; it && in < 63;
             it = it->NextItem, in++) {
            pi = pm;
            path[pi++] = '/';
            path[pi] = '\0';
            ui_item_text(it, path, &pi, sizeof(path) - 2);
            if (!it->SubItem) {
                if (used + 300 > cap) { *full = TRUE; return used; }
                used = ui_menu_line(buf, used, m, NULL, it,
                                    mn | (in << 5) | (31UL << 11), path);
                continue;
            }
            for (sub = it->SubItem, sn = 0; sub && sn < 31;
                 sub = sub->NextItem, sn++) {
                pn = pi;
                path[pn++] = '/';
                path[pn] = '\0';
                ui_item_text(sub, path, &pn, sizeof(path));
                if (used + 300 > cap) { *full = TRUE; return used; }
                used = ui_menu_line(buf, used, m, it, sub,
                                    mn | (in << 5) | (sn << 11), path);
            }
        }
    }
    return used;
}

/* A gadget's box relative to its window, resolved for the REL flags
 * (Intuition's own `width + LeftEdge - 1`) and a GimmeZeroZero window's
 * inner layer. FALSE when it has no area to click. */
static BOOL ui_box(struct Window *w, struct Gadget *g, LONG *x, LONG *y,
                   LONG *gw, LONG *gh)
{
    LONG cw = w->Width, ch = w->Height, ox = 0, oy = 0;

    if ((w->Flags & WFLG_GIMMEZEROZERO) &&
        !(g->GadgetType & GTYP_GZZGADGET)) {
        cw = w->GZZWidth;
        ch = w->GZZHeight;
        ox = w->BorderLeft;
        oy = w->BorderTop;
    }
    *x  = ((g->Flags & GFLG_RELRIGHT)  ? cw + g->LeftEdge - 1 : g->LeftEdge)
          + ox;
    *y  = ((g->Flags & GFLG_RELBOTTOM) ? ch + g->TopEdge - 1  : g->TopEdge)
          + oy;
    *gw = (g->Flags & GFLG_RELWIDTH)  ? cw + g->Width  : g->Width;
    *gh = (g->Flags & GFLG_RELHEIGHT) ? ch + g->Height : g->Height;
    return *gw > 0 && *gh > 0;
}

/*
 * Everything clickable in a window, as UiItems: its Intuition gadgets,
 * the children inside a ReAction layout.gadget, and the objects of an
 * MUI window. One collector, two users: UI (lines for the client) and
 * the WASABI ARexx port's CLICKGADGET.
 */
struct UiItem {
    ULONG addr;                      /* gadget or object */
    LONG  x, y, w, h;                /* relative to the window */
    const char *kind;
    BOOL  disabled, selected;
    UWORD id;
    char  cls[40], label[120], value[120];
};

/* FALSE from a sink: stop, the buffer is full. */
typedef BOOL (*UiSink)(struct UiItem *it, void *ctx);

/* A BOOPSI object we may read: header and class in memory that exists,
 * a dispatcher that does too. */
static BOOL ui_obj_ok(const void *o)
{
    struct IClass *c;
    if (!ui_readable(o) || !ui_readable((const UBYTE *)o -
                                        sizeof(struct _Object)))
        return FALSE;
    c = OCLASS(o);
    return ui_readable(c) && ui_readable(c->cl_Dispatcher.h_Entry);
}

/*
 * MUI 3.x's built-in classes have no cl_ID (MUI 4/5 name theirs), so
 * they are known by address: muimaster.library's MUI_GetClass() (V8,
 * obsolete but there) hands each one over, once, while nothing is
 * locked. Only when an MUI older than 20 is already in memory - then
 * MUI 3.x windows may be open, and nothing is loaded for a system that
 * has MUI installed but is not using it.
 */
#define MUI_GetClass(id) \
    LP1(0x4e, struct IClass *, MUI_GetClass, CONST_STRPTR, id, a0, \
        , g_muimaster)
#define MUI_FreeClass(cl) \
    LP1NR(0x54, MUI_FreeClass, struct IClass *, cl, a0, , g_muimaster)

static struct Library *g_muimaster;      /* MUI 3.x only, see above */
static const char *const g_mui3_names[] = {
    "Window.mui", "Area.mui", "Group.mui", "Text.mui", "String.mui",
    "Cycle.mui", "List.mui", "Numeric.mui"
};
#define MUI3_CLASSES (sizeof(g_mui3_names) / sizeof(g_mui3_names[0]))
static struct IClass *g_mui3_class[MUI3_CLASSES];

static void mui3_open(void)
{
    struct Library *lib;
    ULONG i;

    if (g_muimaster)
        return;
    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, "muimaster.library");
    if (lib && lib->lib_Version >= 20)
        lib = NULL;                      /* MUI 4/5: classes have names */
    Permit();
    if (!lib || !(g_muimaster = OpenLibrary("muimaster.library", 0)))
        return;
    for (i = 0; i < MUI3_CLASSES; i++)
        g_mui3_class[i] = MUI_GetClass((CONST_STRPTR)g_mui3_names[i]);
}

static void mui3_close(void)
{
    ULONG i;

    if (!g_muimaster)
        return;
    for (i = 0; i < MUI3_CLASSES; i++)
        if (g_mui3_class[i])
            MUI_FreeClass(g_mui3_class[i]);
    CloseLibrary(g_muimaster);
    g_muimaster = NULL;
}

/* A class's name: its cl_ID, or the MUI 3.x name it is known by. */
static const char *ui_cl_name(struct IClass *c)
{
    ULONG i;

    if (c->cl_ID && ui_readable(c->cl_ID))
        return (const char *)c->cl_ID;
    if (g_muimaster)
        for (i = 0; i < MUI3_CLASSES; i++)
            if (c == g_mui3_class[i])
                return g_mui3_names[i];
    return NULL;
}

static BOOL ui_class_is(const void *o, const char *name)
{
    struct IClass *c;
    const char *id;
    int k = 0;
    for (c = OCLASS(o); ui_readable(c) && k < 16; c = c->cl_Super, k++)
        if ((id = ui_cl_name(c)) && strcmp(id, name) == 0)
            return TRUE;
    return FALSE;
}

/* The nearest named class: a private subclass shows its public parent. */
static void ui_class_name(const void *o, char *out, LONG sz)
{
    struct IClass *c;
    int k = 0;
    LONG n = 0;
    out[0] = '\0';
    for (c = OCLASS(o); ui_readable(c) && k < 16; c = c->cl_Super, k++)
        if (ui_cl_name(c)) {
            ui_put(out, &n, sz, (const UBYTE *)ui_cl_name(c));
            return;
        }
}

/* An Intuition gadget (plain, GadTools, or a BOOPSI one) as an item. */
static BOOL ui_gadget_item(struct Window *w, struct Gadget *g,
                           struct UiItem *it)
{
    if (!ui_box(w, g, &it->x, &it->y, &it->w, &it->h))
        return FALSE;
    it->addr = (ULONG)g;
    it->kind = ui_kind(g);
    it->disabled = (g->Flags & GFLG_DISABLED) != 0;
    it->selected = (g->Flags & GFLG_SELECTED) != 0;
    it->id = g->GadgetID;
    ui_label(g, it->label, sizeof(it->label));
    it->value[0] = '\0';
    {
        const UBYTE *gt = ui_gt_text(g);
        LONG ln = 0;
        if (gt && !it->label[0])
            ui_putx(it->label, &ln, sizeof(it->label), gt, TRUE);
        else if (gt && strcmp((const char *)gt, it->label) != 0)
            ui_put(it->value, &ln, sizeof(it->value), gt);  /* cycle */
    }
    if ((g->GadgetType & GTYP_GTYPEMASK) == GTYP_STRGADGET &&
        !(g->GadgetType & GTYP_SYSGADGET) && ui_readable(g->SpecialInfo)) {
        LONG vn = 0;
        ui_put(it->value, &vn, sizeof(it->value),
               ((struct StringInfo *)g->SpecialInfo)->Buffer);
    } else if ((g->GadgetType & GTYP_GTYPEMASK) == GTYP_PROPGADGET &&
               !(g->GadgetType & GTYP_SYSGADGET) && g->SpecialInfo) {
        struct PropInfo *pi = (struct PropInfo *)g->SpecialInfo;
        sprintf(it->value, "%u", (unsigned)((pi->Flags & FREEVERT) ?
                                            pi->VertPot : pi->HorizPot));
    }
    it->cls[0] = '\0';
    if ((g->GadgetType & GTYP_GTYPEMASK) == GTYP_CUSTOMGADGET &&
        !(g->GadgetType & GTYP_SYSGADGET) && ui_obj_ok(g))
        ui_class_name(g, it->cls, sizeof(it->cls));
    return TRUE;
}

/* --- list rows --------------------------------------------------------- */

#define ROWS_MAX 100                 /* rows reported per list */

/* A row as an item: kind "row", its text, a box when it is in view
 * (0x0 when scrolled away - it cannot be clicked until it is shown),
 * its index as the value. */
static BOOL ui_row(UiSink sink, void *ctx, ULONG list, LONG i, BOOL sel,
                   const char *text, LONG x, LONG y, LONG w, LONG h)
{
    struct UiItem it;
    LONG n = 0;
    memset(&it, 0, sizeof(it));
    it.addr = list;
    it.kind = "row";
    it.selected = sel;
    it.x = x; it.y = y; it.w = w; it.h = h;
    ui_put(it.label, &n, sizeof(it.label), (const UBYTE *)text);
    sprintf(it.value, "%ld", (long)i);
    return sink(&it, ctx);
}

/*
 * A GadTools listview's rows, through the public GT_GetGadgetAttrs()
 * (V39): its label list, the top row and the selected one. Rows are
 * drawn one font height apart inside a 2-pixel border.
 */
static BOOL ui_gt_rows(struct Window *w, struct Gadget *g, struct UiItem *box,
                       UiSink sink, void *ctx)
{
    struct List *labels = NULL;
    ULONG top = 0, sel = ~0UL, ih = 0;
    struct Node *nd;
    LONG i, rowh, vis;

    if (!(g->GadgetType & GTYP_GADTOOLS_FLAG))
        return TRUE;
    if (!g_gadtools)
        g_gadtools = OpenLibrary("gadtools.library", 39);
    if (!g_gadtools)
        return TRUE;
    if (GT_GetGadgetAttrs(g, w, NULL, GTLV_Labels, (ULONG)&labels,
                          GTLV_Top, (ULONG)&top, GTLV_Selected, (ULONG)&sel,
                          GTLV_ItemHeight, (ULONG)&ih, TAG_DONE) < 1 ||
        !labels || labels == (struct List *)~0UL || !ui_readable(labels))
        return TRUE;
    rowh = ih ? (LONG)ih : (w->RPort ? w->RPort->TxHeight : 8);
    if (rowh <= 0)
        rowh = 8;
    vis = (box->h - 4) / rowh;
    for (nd = labels->lh_Head, i = 0;
         ui_readable(nd) && nd->ln_Succ && i < ROWS_MAX;
         nd = nd->ln_Succ, i++) {
        BOOL in = i >= (LONG)top && i < (LONG)top + vis;
        if (!ui_row(sink, ctx, (ULONG)g, i, (ULONG)i == sel,
                    ui_printable((const UBYTE *)nd->ln_Name) ? nd->ln_Name
                                                              : "",
                    in ? box->x + 2 : 0, in ? box->y + 2 +
                    (i - (LONG)top) * rowh : 0,
                    in ? box->w - 4 : 0, in ? rowh : 0))
            return FALSE;
    }
    return TRUE;
}

/* --- ReAction -------------------------------------------------------- */

/*
 * A layout.gadget keeps its children in an exec MinList inside its own
 * instance data, one node per child with the child object in it (found
 * on OS 3.2's layout.gadget 47.61: the list at +56, the object at node
 * +14). None of that is documented, so nothing is assumed: the list is
 * found by its shape (head, NULL, tailpred), and the object's place in
 * the node by trying each word until every node holds a BOOPSI gadget
 * or image there. Read-only, every pointer checked; a version that
 * keeps its children another way simply shows its layout as before.
 */
static LONG ui_react_offset(const struct MinNode *n)
{
    LONG j;
    for (j = 8; j <= 40; j += 2) {
        const void *o = *(const void * const *)((const UBYTE *)n + j);
        if (ui_obj_ok(o) && (ui_class_is(o, "gadgetclass") ||
                             ui_class_is(o, "imageclass")))
            return j;
    }
    return -1;
}

static BOOL ui_reaction(struct Window *w, const void *lay, int depth,
                        UiSink sink, void *ctx, int *count)
{
    struct IClass *c;
    const UBYTE *d;
    LONG i, size;
    int k = 0;

    for (c = OCLASS(lay); ui_readable(c) && k < 16; c = c->cl_Super, k++)
        if (c->cl_ID && ui_readable(c->cl_ID) &&
            strcmp((const char *)c->cl_ID, "layout.gadget") == 0)
            break;
    if (!ui_readable(c) || k >= 16 || depth > 8)
        return TRUE;
    d = (const UBYTE *)lay + c->cl_InstOffset;
    size = c->cl_InstSize;
    for (i = 0; i + 12 <= size && i < 512; i += 2) {
        const ULONG *e = (const ULONG *)(d + i);
        const struct MinNode *n;
        LONG j, nodes = 0;
        if (!ui_readable((void *)e[0]) || e[1] != 0 ||
            !ui_readable((void *)e[2]) || e[0] == (ULONG)&e[1])
            continue;                    /* not a (non-empty) MinList */
        n = (const struct MinNode *)e[0];
        if ((j = ui_react_offset(n)) < 0)
            continue;
        for (; ui_readable(n) && n->mln_Succ && nodes < 200;
             n = n->mln_Succ, nodes++) {
            const void *o = *(const void * const *)((const UBYTE *)n + j);
            struct UiItem it;
            if (!ui_obj_ok(o))
                continue;
            if (ui_class_is(o, "layout.gadget")) {
                if (!ui_reaction(w, o, depth + 1, sink, ctx, count))
                    return FALSE;
                continue;
            }
            if (!ui_class_is(o, "gadgetclass") || ++*count > 400)
                continue;
            if (ui_gadget_item(w, (struct Gadget *)o, &it) &&
                !sink(&it, ctx))
                return FALSE;
        }
        return TRUE;                     /* the children list: done */
    }
    return TRUE;
}

/* --- MUI ------------------------------------------------------------- */

#define MUIA_Window_RootObject 0x8042cba5
#define MUIA_Window_Window     0x80426a42
#define MUIA_Group_ChildList   0x80424748
#define MUIA_Text_Contents     0x8042f8dc
#define MUIA_String_Contents   0x80428ffd
#define MUIA_InputMode         0x8042fb04
#define MUIA_Selected          0x8042654b
#define MUIA_Disabled          0x80423661
#define MUIA_Cycle_Active      0x80421788
#define MUIA_Cycle_Entries     0x80420629
#define MUIA_List_Entries      0x80421654
#define MUIA_List_First        0x804238d4
#define MUIA_List_Visible      0x8042191f
#define MUIA_List_Active       0x8042391c
#define MUIA_List_DisplayHook  0x8042b4d5
#define MUIA_ObjectID          0x8042d76e
#define MUIM_List_GetEntry     0x804280ec
#define MUIM_List_TestPos      0x80425f48

struct MuiTestPos {                  /* struct MUI_List_TestPos_Result */
    LONG  entry;
    WORD  column;
    UWORD flags;
    WORD  xoffset;
    WORD  yoffset;                   /* from the middle of the row */
};

/* MUI's text: escapes (ESC c, ESC b, ESC I[...]) and the shortcut
 * underscore out. */
static void ui_mui_text(const UBYTE *s, char *out, LONG sz)
{
    LONG n = 0;
    int k = 0;
    if (!ui_readable_text(s))
        s = (const UBYTE *)"";
    for (; *s && n < sz - 1 && k < 400; s++, k++) {
        if (*s == 27) {
            if (!s[1])
                break;
            s++;
            if ((*s == 'I' || *s == 'O' || *s == 'P') && s[1] == '[') {
                while (*s && *s != ']')
                    s++;
                if (!*s)
                    break;
            }
            continue;
        }
        if (*s == '_')
            continue;
        out[n++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

/*
 * From an Intuition window to its MUI window object. MUI keeps no
 * pointer back that is documented, but every MUI object's MUI_RenderInfo
 * (documented: WindowObject, Screen, DrawInfo, Pens, Window) holds both:
 * so the memory is searched for this window's address with its screen
 * one word before and an object of Window.mui one word before that,
 * which MUI then confirms (MUIA_Window_Window). Found once per window,
 * then kept; a kept answer is checked again before it is used.
 */
#define MUI_CACHE 32
/* o NULL: known not to be an MUI 3.x window (mui3_prescan), as long as
 * the window at that address still has the same IDCMP port. */
static struct { struct Window *w; struct MsgPort *port; Object *o; } g_mui[MUI_CACHE];
static LONG g_mui_next;

static BOOL ui_mui_is(Object *o, struct Window *w)
{
    ULONG ww = 0;
    return ui_obj_ok(o) && ui_class_is(o, "Window.mui") &&
           GetAttr(MUIA_Window_Window, o, &ww) && ww == (ULONG)w;
}

/* Plain reads only, so multitasking runs on during the search; the
 * caller confirms the answer with ui_mui_is() under Forbid. */
static BOOL ui_mui_cheap(Object *o)
{
    return ui_obj_ok(o) && ui_class_is(o, "Window.mui");
}

static Object *ui_mui_window(struct Window *w)
{
    struct MemHeader *mh;
    LONG i;

    for (i = 0; i < MUI_CACHE; i++)
        if (g_mui[i].w == w) {
            if (g_mui[i].o && ui_mui_cheap(g_mui[i].o))
                return g_mui[i].o;
            g_mui[i].w = NULL;           /* gone or reused: search again */
        }
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        ULONG *p = (ULONG *)(((ULONG)mh->mh_Lower + 19) & ~3UL);
        ULONG *e = (ULONG *)((ULONG)mh->mh_Upper & ~3UL);
        for (; p < e; p++)
            if (*p == (ULONG)w && p[-3] == (ULONG)w->WScreen &&
                ui_mui_cheap((Object *)p[-4])) {
                g_mui[g_mui_next].w = w;
                g_mui[g_mui_next].port = w->UserPort;
                g_mui[g_mui_next].o = (Object *)p[-4];
                g_mui_next = (g_mui_next + 1) % MUI_CACHE;
                return (Object *)p[-4];
            }
    }
    return NULL;
}

/*
 * MUI 3.x leaves no mark on its windows, so with an MUI 3.x in memory
 * every window not seen before is looked for in one pass over memory
 * for all of them, before Intuition is locked (a pass under LockIBase
 * would freeze the machine for its length); the answers, yes or no, are
 * kept per window. 2 MB on a stock A1200: well under a second.
 */
static LONG mui3_slot(struct Window *w)
{
    LONG i;
    for (i = 0; i < MUI_CACHE; i++)
        if (g_mui[i].w == w && g_mui[i].port == w->UserPort)
            return i;
    return -1;
}

#define MUI3_SKIPS 256                /* free chunks skipped per region */
static void mui3_prescan(void)
{
    struct Window *want[MUI_CACHE];
    struct Screen *scr[MUI_CACHE];
    Object *found[MUI_CACHE];
    struct MemHeader *mh;
    struct Screen *sc;
    struct Window *w;
    static ULONG skip_lo[MUI3_SKIPS], skip_hi[MUI3_SKIPS];
    ULONG ib, lo = ~0UL, hi = 0;
    LONG n = 0, i, left;

    mui3_open();
    if (!g_muimaster || !g_mui3_class[0])
        return;
    ib = LockIBase(0);
    for (sc = IntuitionBase->FirstScreen; sc; sc = sc->NextScreen)
        for (w = sc->FirstWindow; w && n < MUI_CACHE / 2; w = w->NextWindow)
            if (mui3_slot(w) < 0) {
                want[n] = w;
                scr[n] = w->WScreen;
                found[n] = NULL;
                if ((ULONG)w < lo) lo = (ULONG)w;
                if ((ULONG)w > hi) hi = (ULONG)w;
                n++;
            }
    UnlockIBase(ib);
    left = n;
    if (!n)
        return;
    /* Free memory cannot hold the RenderInfo of a window that is open
     * now, so the big free chunks (most of a machine's RAM) are noted
     * under Forbid and skipped; exec keeps them in address order. */
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ && left > 0;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        struct MemChunk *mc;
        ULONG *p = (ULONG *)(((ULONG)mh->mh_Lower + 19) & ~3UL);
        ULONG *e = (ULONG *)((ULONG)mh->mh_Upper & ~3UL);
        LONG f = 0, k;
        Forbid();
        for (mc = mh->mh_First; mc && f < MUI3_SKIPS; mc = mc->mc_Next)
            if (mc->mc_Bytes >= 4096) {
                skip_lo[f] = (ULONG)mc;
                skip_hi[f++] = (ULONG)mc + mc->mc_Bytes;
            }
        Permit();
        for (k = 0; k <= f && left > 0; k++) {
            ULONG *end = k < f ? (ULONG *)skip_lo[k] : e;
            for (; p < end; p++) {
                if (*p < lo || *p > hi)
                    continue;
                for (i = 0; i < n; i++)
                    if (*p == (ULONG)want[i] && !found[i] &&
                        p[-3] == (ULONG)scr[i] &&
                        ui_mui_cheap((Object *)p[-4])) {
                        found[i] = (Object *)p[-4];
                        left--;          /* all found: stop early */
                    }
            }
            if (k < f && (ULONG *)(((skip_hi[k] + 3) & ~3UL) + 16) > p)
                p = (ULONG *)(((skip_hi[k] + 3) & ~3UL) + 16);
        }
    }
    /* The windows may have changed meanwhile: what is kept is checked
     * again where it is used (ui_mui_window, ui_mui_is). */
    ib = LockIBase(0);
    for (i = 0; i < n; i++) {
        g_mui[g_mui_next].w = want[i];
        g_mui[g_mui_next].port = want[i]->UserPort;
        g_mui[g_mui_next].o = found[i];
        g_mui_next = (g_mui_next + 1) % MUI_CACHE;
    }
    UnlockIBase(ib);
}

/* An MUI 3.x window found by mui3_prescan. */
static BOOL mui3_known(struct Window *w)
{
    LONG i = mui3_slot(w);
    return i >= 0 && g_mui[i].o != NULL;
}

/* MUI 4/5 put their own gadgets in the title bar, drawn with an image
 * of class muiborderimage: a sign worth a search, which a window
 * without it does not get unless it is asked for by name. */
static BOOL ui_mui_marked(struct Window *w)
{
    struct Gadget *g;
    int k = 0;
    for (g = w->FirstGadget; g && k < 64; g = g->NextGadget, k++)
        if (ui_obj_ok(g->GadgetRender) &&
            ui_class_is(g->GadgetRender, "muiborderimage"))
            return TRUE;
    return FALSE;
}

struct MuiWalk {
    UiSink sink;
    void *ctx;
    int count;
    char pending[120];               /* the last label, for the next
                                      * unlabelled control */
    LONG px, py, pw, ph;             /* where that label is */
    struct UiItem held;              /* an unlabelled control, waiting to
                                      * see if a label comes after it */
    char fallback[120];              /* the label before it, if no
                                      * label comes after */
    char next_fallback[120];         /* the same for the item being
                                      * emitted, until it is held */
    BOOL has_held;
};

/* b starts on a's row (their boxes overlap in height), right of a. */
static BOOL mw_right_of(LONG ax, LONG ay, LONG aw, LONG ah, struct UiItem *b)
{
    return b->x >= ax + aw && b->y < ay + ah && b->y + b->h > ay;
}

static BOOL mw_flush(struct MuiWalk *mw)
{
    if (!mw->has_held)
        return TRUE;
    mw->has_held = FALSE;
    if (!mw->held.label[0])
        copystr(mw->held.label, sizeof(mw->held.label), mw->fallback);
    return mw->sink(&mw->held, mw->ctx);
}

/* MUI lays a label out before its control or after it (a checkmark,
 * then "Editable"): an unlabelled control waits for the next item, and
 * takes it as its label if that is plain text on the same row to its
 * right and not itself a "Name:" for what follows; otherwise it keeps
 * the label that came before it ("Show: [x] Icon [ ] Menu" is Icon and
 * Menu, "Pop to front: [x]" is Pop to front). */
static BOOL mw_emit(struct MuiWalk *mw, struct UiItem *it)
{
    BOOL holdable = !it->label[0] &&
        (strcmp(it->kind, "toggle") == 0 || strcmp(it->kind, "string") == 0 ||
         strcmp(it->kind, "cycle") == 0 || strcmp(it->kind, "slider") == 0);
    LONG ln = strlen(it->label);
    if (strcmp(it->kind, "label") == 0 && mw->has_held &&
        ln && it->label[ln - 1] != ':' &&
        mw_right_of(mw->held.x, mw->held.y, mw->held.w, mw->held.h, it)) {
        copystr(mw->held.label, sizeof(mw->held.label), it->label);
        if (!mw_flush(mw))
            return FALSE;
        return mw->sink(it, mw->ctx);
    }
    if (!mw_flush(mw))
        return FALSE;
    if (holdable) {
        mw->held = *it;
        copystr(mw->fallback, sizeof(mw->fallback), mw->next_fallback);
        mw->has_held = TRUE;
        return TRUE;
    }
    return mw->sink(it, mw->ctx);
}

/*
 * An MUI list's rows: MUIM_List_GetEntry for each, the text the entry
 * is when it is a string, and otherwise what the list's own display
 * hook makes of it - the app's code, called as MUI calls it (columns
 * into an array), here under Forbid like the rest of the walk. Rows
 * are placed by dividing the list's inner height by the rows it shows.
 */
static BOOL ui_mui_rows(Object *o, struct UiItem *list, struct MuiWalk *mw)
{
    ULONG entries = 0, active = ~0UL;
    struct Hook *hook = NULL;
    static LONG mid[ROWS_MAX];
    static char text[120];
    static STRPTR cols[34];
    LONG i, y, rowh = 0, prev = -1;

    GetAttr(MUIA_List_Entries, o, &entries);
    GetAttr(MUIA_List_Active, o, &active);
    GetAttr(MUIA_List_DisplayHook, o, (ULONG *)&hook);

    /* Where each row is drawn: MUI says which row is under a point and
     * how far that point is from the row's middle (MUIM_List_TestPos). */
    for (i = 0; i < ROWS_MAX; i++)
        mid[i] = -1;
    for (y = list->y; y < list->y + list->h; y += 2) {
        struct MuiTestPos r;
        ULONG msg[4];
        memset(&r, 0, sizeof(r));
        r.entry = -1;
        msg[0] = MUIM_List_TestPos;
        msg[1] = (ULONG)(list->x + list->w / 4);
        msg[2] = (ULONG)y;
        msg[3] = (ULONG)&r;
        DoMethodA(o, (Msg)msg);
        if (r.entry >= 0 && r.entry < ROWS_MAX && mid[r.entry] < 0) {
            mid[r.entry] = y - r.yoffset;
            if (prev >= 0 && r.entry == prev + 1 && !rowh)
                rowh = mid[r.entry] - mid[prev];
            prev = r.entry;
        }
    }
    if (rowh <= 0)
        rowh = 10;

    for (i = 0; i < (LONG)entries && i < ROWS_MAX; i++) {
        APTR e = NULL;
        ULONG msg[3];
        msg[0] = MUIM_List_GetEntry;
        msg[1] = (ULONG)i;
        msg[2] = (ULONG)&e;
        DoMethodA(o, (Msg)msg);
        text[0] = '\0';
        /* The list's own display hook first: an entry is often a record
         * whose first bytes happen to be text (one column of several). */
        if (e && ui_readable(hook) && ui_readable(hook->h_Entry)) {
            LONG k, n = 0;
            memset(cols, 0, sizeof(cols));
            CallHookA(hook, (Object *)(cols + 1), e);
            for (k = 1; k < 33 && n < (LONG)sizeof(text) - 4; k++) {
                char part[80];
                if (!cols[k] || !ui_readable_text(cols[k]))
                    continue;
                ui_mui_text((const UBYTE *)cols[k], part, sizeof(part));
                if (!part[0])
                    continue;
                if (n) {
                    text[n++] = ' ';
                    text[n++] = ' ';
                    text[n] = '\0';
                }
                copystr(text + n, sizeof(text) - n, part);
                n += strlen(text + n);
            }
        }
        if (!text[0] && ui_printable((const UBYTE *)e))
            ui_mui_text((const UBYTE *)e, text, sizeof(text));
        if (!ui_row(mw->sink, mw->ctx, (ULONG)o, i, (ULONG)i == active, text,
                    mid[i] >= 0 ? list->x : 0,
                    mid[i] >= 0 ? mid[i] - rowh / 2 : 0,
                    mid[i] >= 0 ? list->w : 0, mid[i] >= 0 ? rowh : 0))
            return FALSE;
    }
    return TRUE;
}

/*
 * MUI 3.x will not give MUIA_InputMode back (it is init-only there), so
 * it is read from where the object keeps it, found by comparing buttons,
 * checkmarks and labels in MUI 3.8's own windows (muimaster 19.31):
 * after the documented MUI_AreaData (mad_Flags at +64 from the object)
 * comes the IDCMP the object asked for - IDCMP_MOUSEBUTTONS for anything
 * that is clicked - and a toggle is the one of those with bit 16 of
 * mad_Flags clear (set on buttons and on everything else alike).
 * Answers 0 (none), 1 (button) or 3 (toggle), as MUIA_InputMode would.
 */
static ULONG mui3_input_mode(Object *o)
{
    const ULONG *ad = (const ULONG *)((const UBYTE *)o + 64);
    if (!ui_readable(ad + 3) || !(ad[3] & IDCMP_MOUSEBUTTONS))
        return 0;
    return (ad[0] & 0x10000) ? 1 : 3;
}

static BOOL ui_mui_walk(Object *o, int depth, struct MuiWalk *mw)
{
    struct UiItem it;
    const WORD *box = (const WORD *)((const UBYTE *)o + 52);  /* _left.. */
    ULONG v = 0;

    if (depth > 14 || ++mw->count > 400 || !ui_obj_ok(o) ||
        !ui_class_is(o, "Area.mui"))
        return TRUE;
    memset(&it, 0, sizeof(it));
    it.addr = (ULONG)o;
    it.x = box[0];
    it.y = box[1];
    it.w = box[2];
    it.h = box[3];
    ui_class_name(o, it.cls, sizeof(it.cls));
    if (GetAttr(MUIA_Disabled, o, &v) && v)
        it.disabled = TRUE;

    if (ui_class_is(o, "String.mui")) {
        it.kind = "string";
        if (GetAttr(MUIA_String_Contents, o, &v))
            ui_mui_text((const UBYTE *)v, it.value, sizeof(it.value));
    } else if (ui_class_is(o, "Cycle.mui")) {
        ULONG e = 0;
        it.kind = "cycle";
        if (GetAttr(MUIA_Cycle_Active, o, &v) &&
            GetAttr(MUIA_Cycle_Entries, o, &e) && ui_readable((void *)e) &&
            v < 64)
            ui_mui_text((const UBYTE *)((ULONG *)e)[v], it.value,
                        sizeof(it.value));
    } else if (ui_class_is(o, "List.mui")) {
        it.kind = "list";
        if (it.w > 0 && it.h > 0) {
            if (!mw_emit(mw, &it))
                return FALSE;
            return ui_mui_rows(o, &it, mw);
        }
    } else if (ui_class_is(o, "Numeric.mui")) {
        it.kind = "slider";
    } else if (ui_class_is(o, "Group.mui")) {
        struct List *l = NULL;
        struct Node *n;
        int k = 0;
        if (it.w <= 0 || it.h <= 0)
            return TRUE;                 /* a page not shown: nor are its
                                          * children, and they would use
                                          * up the 400 */
        if (!GetAttr(MUIA_Group_ChildList, o, (ULONG *)&l) ||
            !ui_readable(l))
            return TRUE;
        for (n = l->lh_Head; ui_readable(n) && n->ln_Succ && k < 200;
             n = n->ln_Succ, k++)
            if (!ui_mui_walk((Object *)((struct _Object *)n + 1), depth + 1,
                             mw))
                return FALSE;
        return TRUE;
    } else {
        ULONG mode = 0;
        if (g_muimaster)
            mode = mui3_input_mode(o);
        else
            GetAttr(MUIA_InputMode, o, &mode);
        if (ui_class_is(o, "Text.mui") &&
            GetAttr(MUIA_Text_Contents, o, &v))
            ui_mui_text((const UBYTE *)v, it.label, sizeof(it.label));
        it.kind = mode == 1 ? "button" : mode == 3 ? "toggle" :
                  mode == 2 ? "button" : "label";
        if (mode && GetAttr(MUIA_Selected, o, &v) && v)
            it.selected = TRUE;
        if (!mode) {                     /* a label: remember it */
            if (it.label[0]) {
                copystr(mw->pending, sizeof(mw->pending), it.label);
                mw->px = it.x; mw->py = it.y; mw->pw = it.w; mw->ph = it.h;
            }
            if (!it.label[0] || it.w <= 0 || it.h <= 0)
                return TRUE;
            return mw_emit(mw, &it);
        }
    }
    if (it.w <= 0 || it.h <= 0)
        return TRUE;                     /* hidden: a page not shown */
    /* A control MUI lays out after its Label: string, cycle, slider,
     * checkmark. A button has its own text, a list its own title. */
    mw->next_fallback[0] = '\0';
    if (!it.label[0] && mw->pending[0] && strcmp(it.kind, "toggle") == 0)
        copystr(mw->next_fallback, sizeof(mw->next_fallback), mw->pending);
    else if (!it.label[0] && mw->pending[0] &&
        (strcmp(it.kind, "string") == 0 || strcmp(it.kind, "cycle") == 0 ||
         strcmp(it.kind, "slider") == 0))
        copystr(it.label, sizeof(it.label), mw->pending);
    mw->pending[0] = '\0';
    /* MUI objects are not gadgets and have no GadgetID; the program's
     * own MUIA_ObjectID (often set, for its settings) stands in. */
    if (GetAttr(MUIA_ObjectID, o, &v))
        it.id = v;
    return mw_emit(mw, &it);
}

/* --- all of it --------------------------------------------------------- */

/* MUI windows are read when MUI 4/5 marked their title bar, or when
 * mui3_prescan found them (MUI 3.x leaves no mark). `deep` is kept for
 * the protocol (UI flag 2) and does nothing more. */
static BOOL ui_items(struct Window *w, BOOL deep, UiSink sink, void *ctx)
{
    struct Gadget *g;
    struct UiItem it;
    int k = 0, count = 0;

    for (g = w->FirstGadget; g && k < 400; g = g->NextGadget, k++) {
        if ((g->GadgetType & GTYP_GTYPEMASK) == GTYP_CUSTOMGADGET &&
            !(g->GadgetType & GTYP_SYSGADGET) && ui_obj_ok(g) &&
            ui_class_is(g, "layout.gadget")) {
            int before = count;
            if (!ui_reaction(w, g, 0, sink, ctx, &count))
                return FALSE;
            if (count > before)
                continue;                /* its children stand for it */
        }
        if (ui_gadget_item(w, g, &it)) {
            if (!sink(&it, ctx) || !ui_gt_rows(w, g, &it, sink, ctx))
                return FALSE;
        }
    }
    /* Requesters in the window: OS 2.x's EasyRequest keeps its buttons
     * here, not among the window's gadgets. A requester gadget's place
     * is relative to the requester, which sits where its layer is. */
    {
        struct Requester *r;
        int rk = 0;
        for (r = w->FirstRequest; r && rk < 8; r = r->OlderRequest, rk++) {
            LONG rx = r->LeftEdge, ry = r->TopEdge;
            if (r->ReqLayer) {
                rx = r->ReqLayer->bounds.MinX - w->LeftEdge;
                ry = r->ReqLayer->bounds.MinY - w->TopEdge;
            }
            for (g = r->ReqGadget; g && k < 400; g = g->NextGadget, k++) {
                if (!ui_gadget_item(w, g, &it))
                    continue;
                it.x = rx + ((g->Flags & GFLG_RELRIGHT) ?
                             r->Width + g->LeftEdge - 1 : g->LeftEdge);
                it.y = ry + ((g->Flags & GFLG_RELBOTTOM) ?
                             r->Height + g->TopEdge - 1 : g->TopEdge);
                it.w = (g->Flags & GFLG_RELWIDTH) ? r->Width + g->Width
                                                  : g->Width;
                it.h = (g->Flags & GFLG_RELHEIGHT) ? r->Height + g->Height
                                                   : g->Height;
                if (it.w > 0 && it.h > 0 && !sink(&it, ctx))
                    return FALSE;
            }
        }
    }
    (void)deep;
    if (ui_mui_marked(w) || mui3_known(w)) {
        Object *mo = ui_mui_window(w), *root = NULL;
        Forbid();                        /* the app must not free them
                                          * while they are read */
        if (mo && ui_mui_is(mo, w) &&
            GetAttr(MUIA_Window_RootObject, mo, (ULONG *)&root) && root) {
            struct MuiWalk mw;
            BOOL go;
            memset(&mw, 0, sizeof(mw));
            mw.sink = sink;
            mw.ctx = ctx;
            go = ui_mui_walk(root, 0, &mw) && mw_flush(&mw);
            Permit();
            return go;
        }
        Permit();
    }
    return TRUE;
}

/* UI's sink: one G line each. */
struct UiBuf {
    char *buf;
    LONG used, cap;
    BOOL full;
};

static BOOL ui_line(struct UiItem *it, void *ctx)
{
    struct UiBuf *b = (struct UiBuf *)ctx;
    char fl[3];
    int f = 0;
    if (b->used + 400 > b->cap) {
        b->full = TRUE;
        return FALSE;
    }
    if (it->disabled) fl[f++] = 'd';
    if (it->selected) fl[f++] = 's';
    if (!f) fl[f++] = '-';
    fl[f] = '\0';
    b->used += sprintf(b->buf + b->used,
                       "G\t0x%08lx\t%ld\t%ld\t%ld\t%ld\t%s\t%s\t%u"
                       "\t%s\t%s\t%s\n",
                       (unsigned long)it->addr, (long)it->x, (long)it->y,
                       (long)it->w, (long)it->h, it->kind, fl,
                       (unsigned)it->id, it->cls[0] ? it->cls : "-",
                       it->label, it->value);
    return TRUE;
}

static BOOL send_lines(int fd, char *buf, LONG used)
{
    LONG i, n;
    BOOL ok = TRUE;

    for (i = 0; ok && i < used; i += n) {
        n = used - i;
        if (n > 8192) {
            n = 8192;
            while (n > 0 && buf[i + n - 1] != '\n')
                n--;
        }
        ok = send_frame(fd, T_DATA, buf + i, n);
    }
    return ok;
}

/* flags bit 0: every screen; otherwise only the front one, which is
 * where a click can land. */
static BOOL cmd_ui(int fd, ULONG flags)
{
    struct Screen *sc;
    struct Window *w, *active;
    char *buf;
    char task[48], title[120];
    LONG used = 0;
    BOOL ok, full = FALSE;
    ULONG ib;

    buf = AllocMem(UI_BUF, MEMF_ANY);
    if (!buf)
        return send_perr(fd, "out of memory");

    mui3_prescan();                      /* before the lock: see there */
    ib = LockIBase(0);
    active = IntuitionBase->ActiveWindow;
    for (sc = IntuitionBase->FirstScreen; sc && !full; sc = sc->NextScreen) {
        copystr(title, sizeof(title), sc->Title ? (char *)sc->Title : "");
        win_clean(title);
        if (used + 300 > UI_BUF) { full = TRUE; break; }
        used += sprintf(buf + used, "S\t0x%08lx\t%ld\t%ld\t%ld\t%ld\t%ld\t%s\t%ld\t%ld\n",
                        (unsigned long)sc, (long)sc->LeftEdge,
                        (long)sc->TopEdge, (long)sc->Width,
                        (long)sc->Height,
                        (long)bm_depth(sc->RastPort.BitMap),
                        title, (long)sc->MouseX, (long)sc->MouseY);
        for (w = sc->FirstWindow; w && !full; w = w->NextWindow) {
            char fl[3];
            LONG f = 0;
            if (used + 300 > UI_BUF) { full = TRUE; break; }
            if (w == active) fl[f++] = 'a';
            if (w->Flags & WFLG_BACKDROP) fl[f++] = 'b';
            if (!f) fl[f++] = '-';
            fl[f] = '\0';
            win_task_name(w, task, sizeof(task));
            win_clean(task);
            copystr(title, sizeof(title), w->Title ? (char *)w->Title : "");
            win_clean(title);
            used += sprintf(buf + used,
                    "W\t0x%08lx\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld"
                    "\t%s\t%s\t%s\n",
                    (unsigned long)w, (long)w->LeftEdge, (long)w->TopEdge,
                    (long)w->Width, (long)w->Height,
                    (long)w->BorderLeft, (long)w->BorderTop,
                    (long)w->BorderRight, (long)w->BorderBottom,
                    fl, task[0] ? task : "-", title);
            {
                struct UiBuf b;
                b.buf = buf;
                b.used = used;
                b.cap = UI_BUF;
                b.full = FALSE;
                ui_items(w, (flags & 2) != 0, ui_line, &b);
                used = b.used;
                full = b.full;
            }
            if (!full)
                used = ui_menus(w, buf, used, UI_BUF, &full);
        }
        if (!(flags & 1))
            break;
    }
    UnlockIBase(ib);

    ok = send_lines(fd, buf, used);
    FreeMem(buf, UI_BUF);
    if (!ok)
        return FALSE;
    if (full)
        return send_perr(fd, "too many gadgets; the list is cut short");
    return send_frame(fd, T_END, NULL, 0);
}

/* --- messages we post and wait for: ARexx and menu picks ----------- */

/*
 * One reply port for every message the daemon sends to somebody else's
 * port. Nothing here waits: the main loop adds the port's signal to its
 * WaitSelect, so a slow ARexx host holds up only the client that asked.
 *
 * A message whose answer is late still belongs to its receiver until it
 * comes back, so a timeout only tells the client; the message stays in
 * g_posted and is freed when it is finally replied. If the daemon exits
 * with some still out, the port is set to PA_IGNORE and left behind -
 * deleting it, or leaving it signalling a task that is gone, would make
 * the late ReplyMsg write into freed memory.
 */
#define MAX_POSTED 16
#define POST_REXX  1
#define POST_MENU  2

struct Posted {
    struct Message *msg;             /* NULL: entry free */
    UBYTE  kind;
    int    client;                   /* -1: nobody waits any more */
    ULONG  deadline;                 /* now_secs() */
};

static struct Posted   g_posted[MAX_POSTED];
static struct MsgPort *g_post_port;
static struct Library *g_rexxsys;

static LONG posted_count(void)
{
    LONG i, n = 0;
    for (i = 0; i < MAX_POSTED; i++)
        if (g_posted[i].msg)
            n++;
    return n;
}

static struct Posted *post_slot(void)
{
    LONG i;
    if (!g_post_port && !(g_post_port = CreateMsgPort()))
        return NULL;
    for (i = 0; i < MAX_POSTED; i++)
        if (!g_posted[i].msg)
            return &g_posted[i];
    return NULL;
}

static ULONG posted_sigmask(void)
{
    return g_post_port ? 1UL << g_post_port->mp_SigBit : 0;
}

static void posted_free(struct Posted *ps)
{
    if (ps->kind == POST_REXX) {
        struct RexxMsg *rm = (struct RexxMsg *)ps->msg;
        if (rm->rm_Args[0])
            DeleteArgstring(rm->rm_Args[0]);
        if (rm->rm_Result1 == 0 && rm->rm_Result2)
            DeleteArgstring((UBYTE *)rm->rm_Result2);
        DeleteRexxMsg(rm);
    } else
        FreeMem(ps->msg, sizeof(struct ExtIntuiMessage));
    ps->msg = NULL;
}

/* AREXX: payload u32 timeout seconds, str port, str command.
 * Answer: DATA = u32 rc, u32 rc2, result bytes; then END. */
static BOOL cmd_arexx(int cl, int fd, const UBYTE *p, LONG len)
{
    /* Static: a kilobyte is an eighth of the shell's 8 KB stack, and the
     * daemon runs on that stack when started at boot (this crashed it
     * in the emulator the first time). */
    static char port[64], cmd[1024];
    struct Posted *ps;
    struct RexxMsg *rm;
    struct MsgPort *dest;
    ULONG secs;

    if (len < 4 || !get_str(p, len, 4, port, sizeof(port)) ||
        !get_str(p, len, 6 + (LONG)strlen(port), cmd, sizeof(cmd)))
        return send_perr(fd, "bad AREXX header");
    secs = get_be32(p);
    if (secs == 0 || secs > 3600)
        secs = 30;
    if (!g_rexxsys)
        g_rexxsys = OpenLibrary("rexxsyslib.library", 36);
    if (!g_rexxsys)
        return send_perr(fd, "cannot open rexxsyslib.library - is ARexx "
                            "installed?");
    if (!(ps = post_slot()))
        return send_perr(fd, "too many ARexx messages are still waiting "
                            "for an answer");
    if (!(rm = CreateRexxMsg(g_post_port, NULL, NULL)))
        return send_perr(fd, "out of memory");
    if (!(rm->rm_Args[0] = CreateArgstring(cmd, (ULONG)strlen(cmd)))) {
        DeleteRexxMsg(rm);
        return send_perr(fd, "out of memory");
    }
    rm->rm_Action = RXCOMM | RXFF_RESULT;

    Forbid();
    dest = FindPort(port);
    if (dest)
        PutMsg(dest, (struct Message *)rm);
    Permit();
    if (!dest) {
        char msg[120];
        DeleteArgstring(rm->rm_Args[0]);
        DeleteRexxMsg(rm);
        sprintf(msg, "no port named \"%.60s\" (port names are case "
                     "sensitive)", port);
        return send_perr(fd, msg);
    }
    ps->msg = (struct Message *)rm;
    ps->kind = POST_REXX;
    ps->client = cl;
    ps->deadline = now_secs() + secs;
    return TRUE;                         /* the answer comes from pump */
}

/*
 * UIACT: payload u32 op, u32 window address, u32 argument.
 *   op 1  pick a menu item: a MENUPICK message to the window's IDCMP
 *         port, argument = the FULLMENUNUM from UI. What the program
 *         then does is exactly what it does when a person picks it;
 *         what it does not get is Intuition's own side effects - a
 *         checkmark item is toggled here, as Intuition would.
 *   op 2  bring the window's screen and the window to the front and
 *         make it the active window, so a click lands on it.
 * The window must still be in Intuition's lists: an address from a
 * minute ago may have been closed and reused since.
 */
static BOOL window_alive(struct Window *want)
{
    struct Screen *sc;
    struct Window *w;
    for (sc = IntuitionBase->FirstScreen; sc; sc = sc->NextScreen)
        for (w = sc->FirstWindow; w; w = w->NextWindow)
            if (w == want)
                return TRUE;
    return FALSE;
}

/* Pick menu item `code` (FULLMENUNUM) in w: NULL, or why not. */
static const char *menu_pick(struct Window *w, ULONG code)
{
    const char *why = NULL;
    ULONG ib;
    {
        struct Posted *ps = post_slot();
        struct ExtIntuiMessage *im;
        struct MenuItem *it;
        ULONG s, m;

        if (!ps)
            return "too many messages are still waiting for an answer";
        im = AllocMem(sizeof(*im), MEMF_PUBLIC | MEMF_CLEAR);
        if (!im)
            return "out of memory";

        ib = LockIBase(0);
        if (!window_alive(w))
            why = "that window is gone";
        else if (!w->UserPort)
            why = "that window has no IDCMP port";
        else if (!(w->IDCMPFlags & IDCMP_MENUPICK))
            why = "that window does not listen for menu picks";
        else if (!w->MenuStrip ||
                 !(it = ItemAddress(w->MenuStrip, (UWORD)code)))
            why = "no such menu item";
        else if (!(it->Flags & ITEMENABLED))
            why = "that menu item is disabled";
        else {
            if (it->Flags & CHECKIT) {
                if ((it->Flags & MENUTOGGLE) || !(it->Flags & CHECKED))
                    it->Flags ^= CHECKED;
                /* Mutual exclusion is the program's to apply on the
                 * next refresh; Intuition does it on release, which
                 * this message stands in for. */
                if (it->MutualExclude && (it->Flags & CHECKED)) {
                    struct Menu *mm = w->MenuStrip;
                    struct MenuItem *o;
                    ULONG mnum = code & 31, k;
                    for (k = 0; mm && k < mnum; k++)
                        mm = mm->NextMenu;
                    o = mm ? mm->FirstItem : NULL;
                    if (((code >> 11) & 31) != 31)
                        o = NULL;        /* sub-items: leave it alone */
                    for (k = 0; o && k < 32; o = o->NextItem, k++)
                        if (o != it && (it->MutualExclude & (1UL << k)))
                            o->Flags &= ~CHECKED;
                }
            }
            it->NextSelect = MENUNULL;
            CurrentTime(&s, &m);
            im->eim_IntuiMessage.ExecMessage.mn_Node.ln_Type = NT_MESSAGE;
            im->eim_IntuiMessage.ExecMessage.mn_ReplyPort = g_post_port;
            im->eim_IntuiMessage.ExecMessage.mn_Length = sizeof(*im);
            im->eim_IntuiMessage.Class = IDCMP_MENUPICK;
            im->eim_IntuiMessage.Code = (UWORD)code;
            im->eim_IntuiMessage.IDCMPWindow = w;
            im->eim_IntuiMessage.MouseX = w->MouseX;
            im->eim_IntuiMessage.MouseY = w->MouseY;
            im->eim_IntuiMessage.Seconds = s;
            im->eim_IntuiMessage.Micros = m;
            PutMsg(w->UserPort, (struct Message *)im);
        }
        UnlockIBase(ib);
        if (why) {
            FreeMem(im, sizeof(*im));
            return why;
        }
        ps->msg = (struct Message *)im;
        ps->kind = POST_MENU;
        ps->client = -1;                 /* answered now, not on reply */
        ps->deadline = 0;
        return NULL;
    }
}

/* Screen, window to the front, and make it the active one. */
static const char *window_front(struct Window *w)
{
    ULONG ib = LockIBase(0);
    BOOL alive = window_alive(w);
    UnlockIBase(ib);
    if (!alive)
        return "that window is gone";
    /* Not under the lock: these queue work for Intuition's own task,
     * which needs that lock to do it. A window closed in the moment
     * between is the remaining risk, as with any tool that holds a
     * window address. */
    ScreenToFront(w->WScreen);
    WindowToFront(w);
    ActivateWindow(w);
    return NULL;
}

static BOOL cmd_uiact(int fd, ULONG op, ULONG waddr, ULONG arg)
{
    const char *why;

    if (op == 1)
        why = menu_pick((struct Window *)waddr, arg);
    else if (op == 2)
        why = window_front((struct Window *)waddr);
    else
        why = "bad UIACT op";
    return why ? send_perr(fd, why) : send_frame(fd, T_OK, NULL, 0);
}

/* Main loop: collect replies, answer who asked, time out the slow. */
static void pump_posted(void)
{
    struct Message *msg;
    LONG i;
    ULONG now;

    if (!g_post_port)
        return;
    while ((msg = GetMsg(g_post_port))) {
        for (i = 0; i < MAX_POSTED; i++)
            if (g_posted[i].msg == msg)
                break;
        if (i == MAX_POSTED)
            continue;                    /* not ours: cannot happen */
        if (g_posted[i].kind == POST_REXX && g_posted[i].client >= 0) {
            struct RexxMsg *rm = (struct RexxMsg *)msg;
            int fd = g_clients[g_posted[i].client].fd;
            /* One DATA frame: rc, rc2, then as much RESULT as fits. */
            UBYTE hdr[8], *out = hdr;
            LONG n = 0, cap = 0;
            if (rm->rm_Result1 == 0 && rm->rm_Result2) {
                cap = LengthArgstring((UBYTE *)rm->rm_Result2);
                if (cap > 32768)
                    cap = 32768;
                if (!(out = AllocMem(8 + cap, MEMF_ANY))) {
                    out = hdr;
                    cap = 0;
                }
            }
            put_be32(out, (ULONG)rm->rm_Result1);
            put_be32(out + 4, rm->rm_Result1 ? (ULONG)rm->rm_Result2 : 0);
            if (cap) {
                n = cap;
                memcpy(out + 8, (UBYTE *)rm->rm_Result2, n);
            }
            if (!send_frame(fd, T_DATA, out, 8 + n) ||
                !send_frame(fd, T_END, NULL, 0))
                drop(g_posted[i].client);
            if (out != hdr)
                FreeMem(out, 8 + cap);
        }
        posted_free(&g_posted[i]);
    }
    now = now_secs();
    for (i = 0; i < MAX_POSTED; i++) {
        struct Posted *ps = &g_posted[i];
        if (ps->msg && ps->client >= 0 && ps->deadline && now > ps->deadline) {
            int cl = ps->client;
            ps->client = -1;             /* the message stays until replied */
            if (!send_perr(g_clients[cl].fd, "the ARexx host did not answer "
                          "in time (it may still act on the command)"))
                drop(cl);
        }
    }
}

/* drop(): nobody is left to tell. */
static void posted_forget(int cl)
{
    LONG i;
    for (i = 0; i < MAX_POSTED; i++)
        if (g_posted[i].client == cl)
            g_posted[i].client = -1;
}

/* Exit: a few seconds for late replies, then leave the port deaf. */
static void posted_shutdown(void)
{
    int w;
    if (!g_post_port)
        return;
    for (w = 0; posted_count() && w < 100; w++) {
        pump_posted();
        if (posted_count())
            Delay(5);
    }
    if (posted_count()) {
        g_post_port->mp_Flags = PA_IGNORE;
        Printf("wasabid: %ld message(s) never came back; their reply port "
               "stays in memory\n", posted_count());
    } else
        DeleteMsgPort(g_post_port);
    g_post_port = NULL;
}

/* PORTS: every public message port, one name per line. */
static BOOL cmd_ports(int fd)
{
    char *buf = AllocMem(16384, MEMF_ANY);
    struct Node *n;
    LONG used = 0;
    BOOL ok, full = FALSE;

    if (!buf)
        return send_perr(fd, "out of memory");
    Forbid();
    for (n = SysBase->PortList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        LONG k = 0;
        if (!n->ln_Name || !n->ln_Name[0])
            continue;
        if (used + 80 > 16384) { full = TRUE; break; }
        ui_put(buf + used, &k, 78, (UBYTE *)n->ln_Name);
        used += k;
        buf[used++] = '\n';
    }
    Permit();
    ok = send_lines(fd, buf, used);
    FreeMem(buf, 16384);
    if (!ok)
        return FALSE;
    if (full)
        return send_perr(fd, "too many ports; the list is cut short");
    return send_frame(fd, T_END, NULL, 0);
}

/* --- the WASABI ARexx port: Amiga programs drive the machine too ----- */

/*
 * The same hands the PC has - type, press keys, click, click a gadget by
 * its label, pick a menu item by name, bring a window forward - offered
 * on this machine as a public ARexx port, so a script or a program can
 * use them without a network or a PC:
 *
 *   address WASABI
 *   'ACTIVATEWINDOW "Shell#?"'
 *   'TYPE "dir<return>"'
 *   'CLICKGADGET Save WINDOW "MyPrefs#?"'
 *   'MENU "Project/Quit"'
 *
 * rc 0 done, 5 nothing matched, 10 refused or failed (LASTERROR says
 * why), 20 not a command. Results (VERSION, WINDOWS, ACTIVATEWINDOW's
 * title) come back in RESULT when the sender asked for one.
 *
 * Only programs on this Amiga reach a public port, and they can do all
 * of this through input.device anyway; the port adds convenience, not
 * reach. A trial instance opens none: the running daemon owns the name.
 */
#define REXX_PORT_NAME "WASABI"

static struct MsgPort *g_rexx_port;
static char g_rx_error[160];

static void rexxport_open(void)
{
    struct MsgPort *mp;

    if (g_rexx_port || g_trial)
        return;
    if (!g_rexxsys)
        g_rexxsys = OpenLibrary("rexxsyslib.library", 36);
    if (!g_rexxsys || !(mp = CreateMsgPort()))
        return;
    mp->mp_Node.ln_Name = REXX_PORT_NAME;
    mp->mp_Node.ln_Pri = 0;
    Forbid();
    if (FindPort(REXX_PORT_NAME)) {      /* another daemon has it */
        Permit();
        DeleteMsgPort(mp);
        return;
    }
    AddPort(mp);
    Permit();
    g_rexx_port = mp;
}

static ULONG rexxport_sigmask(void)
{
    return g_rexx_port ? 1UL << g_rexx_port->mp_SigBit : 0;
}

static void rexxport_close(void)
{
    struct Message *m;

    if (!g_rexx_port)
        return;
    Forbid();
    RemPort(g_rexx_port);                /* nobody finds it from now on */
    Permit();
    while ((m = GetMsg(g_rexx_port))) {
        ((struct RexxMsg *)m)->rm_Result1 = 20;
        ((struct RexxMsg *)m)->rm_Result2 = 0;
        ReplyMsg(m);
    }
    DeleteMsgPort(g_rexx_port);
    g_rexx_port = NULL;
}

/* The next word of *pp into out: "quoted", 'quoted' or bare. FALSE at
 * the end. */
static BOOL rx_word(char **pp, char *out, LONG sz)
{
    char *p = *pp;
    LONG n = 0;
    char q = 0;

    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return FALSE;
    if (*p == '"' || *p == '\'')
        q = *p++;
    while (*p && (q ? *p != q : (*p != ' ' && *p != '\t'))) {
        if (n < sz - 1)
            out[n++] = *p;
        p++;
    }
    if (q && *p == q)
        p++;
    out[n] = '\0';
    *pp = p;
    return TRUE;
}

/* How names are compared: any case, no underscore, no trailing dots,
 * colon or spaces - "Save _As..." is "save as". */
static void rx_norm(const char *in, char *out, LONG sz)
{
    LONG n = 0;
    for (; *in && n < sz - 1; in++) {
        UBYTE c = (UBYTE)*in;
        if (c == '_')
            continue;
        if (c >= 'A' && c <= 'Z')
            c += 32;
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
            c += 32;                     /* Latin-1 capitals */
        if (c == ' ' && (n == 0 || out[n - 1] == ' '))
            continue;
        out[n++] = (char)c;
    }
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ':' ||
                     out[n - 1] == ' '))
        n--;
    out[n] = '\0';
}

/* 0 exact, 1 starts with, 2 contains, -1 no match (both normalized). */
static int rx_tier(const char *have, const char *want)
{
    LONG lw = (LONG)strlen(want);
    if (!want[0] || !have[0])
        return -1;
    if (strcmp(have, want) == 0)
        return 0;
    if (strncmp(have, want, lw) == 0)
        return 1;
    return strstr(have, want) ? 2 : -1;
}

/* The row doubling grab applies, which the pointer's y also follows:
 * 1 for square pixels, 2 for 640x256, 4 for 1280x256. */
static LONG rx_row_scale(struct Screen *sc)
{
    LONG k = 1;
    while (sc->Height && sc->Width >= 2 * sc->Height * k)
        k *= 2;
    return k;
}

/* A window by AmigaDOS pattern on its title, front to back; NULL
 * pattern: the active window. Call under LockIBase. */
static struct Window *rx_find_window(const char *pat)
{
    static char tok[256];
    struct Screen *sc;
    struct Window *w;

    if (!pat)
        return IntuitionBase->ActiveWindow;
    if (ParsePatternNoCase((STRPTR)pat, tok, sizeof(tok)) < 0)
        return NULL;
    for (sc = IntuitionBase->FirstScreen; sc; sc = sc->NextScreen)
        for (w = sc->FirstWindow; w; w = w->NextWindow)
            if (w->Title && MatchPatternNoCase(tok, w->Title))
                return w;
    return NULL;
}

static LONG rx_fail(LONG rc, const char *why)
{
    copystr(g_rx_error, sizeof(g_rx_error), why);
    return rc;
}

/* Key names, as the PC's `wasabi key press` knows them. */
static const struct { const char *name; UBYTE code; } rx_keys[] = {
    { "space", 0x40 }, { "backspace", 0x41 }, { "tab", 0x42 },
    { "enter", 0x43 }, { "return", 0x44 }, { "esc", 0x45 },
    { "escape", 0x45 }, { "del", 0x46 }, { "delete", 0x46 },
    { "up", 0x4C }, { "down", 0x4D }, { "right", 0x4E }, { "left", 0x4F },
    { "help", 0x5F }, { "f1", 0x50 }, { "f2", 0x51 }, { "f3", 0x52 },
    { "f4", 0x53 }, { "f5", 0x54 }, { "f6", 0x55 }, { "f7", 0x56 },
    { "f8", 0x57 }, { "f9", 0x58 }, { "f10", 0x59 },
    /* PC-keyboard keys as OS 3.2/3.9 and AROS number them */
    { "f11", 0x4B }, { "f12", 0x6F }, { "insert", 0x47 }, { "ins", 0x47 },
    { "pageup", 0x48 }, { "pgup", 0x48 }, { "pagedown", 0x49 },
    { "pgdn", 0x49 }, { "home", 0x70 }, { "end", 0x71 },
    /* the numeric keypad */
    { "kp0", 0x0F }, { "kp1", 0x1D }, { "kp2", 0x1E }, { "kp3", 0x1F },
    { "kp4", 0x2D }, { "kp5", 0x2E }, { "kp6", 0x2F }, { "kp7", 0x3D },
    { "kp8", 0x3E }, { "kp9", 0x3F }, { "kpdot", 0x3C }, { "kpenter", 0x43 },
    { "kpminus", 0x4A }, { "kplparen", 0x5A }, { "kprparen", 0x5B },
    { "kpslash", 0x5C }, { "kpstar", 0x5D }, { "kpplus", 0x5E },
    { NULL, 0 }
};
static const struct { const char *name; UBYTE code; UWORD qual; } rx_mods[] = {
    { "shift", 0x60, 0x01 }, { "lshift", 0x60, 0x01 },
    { "rshift", 0x61, 0x02 }, { "ctrl", 0x63, 0x08 }, { "alt", 0x64, 0x10 },
    { "lalt", 0x64, 0x10 }, { "ralt", 0x65, 0x20 }, { "lamiga", 0x66, 0x40 },
    { "ramiga", 0x67, 0x80 }, { "amiga", 0x67, 0x80 },
    { NULL, 0, 0 }
};

/* "ramiga+shift+q" -> raw events (code, qual as 4 bytes each) appended
 * at out[*n]; modifiers down first and up last, as fingers do it. */
static BOOL rx_combo(const char *combo, UBYTE *out, LONG *n, LONG cap)
{
    char part[24], low[24];
    const char *p = combo;
    UBYTE mods[6];
    LONG nm = 0, i, k;
    UWORD qual = 0;
    LONG code = -1;

    while (*p) {
        LONG pn = 0;
        while (*p && *p != '+' && pn < (LONG)sizeof(part) - 1)
            part[pn++] = *p++;
        part[pn] = '\0';
        if (*p == '+')
            p++;
        rx_norm(part, low, sizeof(low));
        if (*p) {                        /* not the last: a modifier */
            for (k = 0; rx_mods[k].name; k++)
                if (strcmp(low, rx_mods[k].name) == 0)
                    break;
            if (!rx_mods[k].name || nm >= 6)
                return FALSE;
            mods[nm++] = (UBYTE)k;
            continue;
        }
        for (k = 0; rx_keys[k].name; k++)
            if (strcmp(low, rx_keys[k].name) == 0)
                code = rx_keys[k].code;
        for (k = 0; code < 0 && rx_mods[k].name; k++)
            if (strcmp(low, rx_mods[k].name) == 0)
                code = rx_mods[k].code;
        if (code < 0 && low[0] == '0' && low[1] == 'x') {
            /* a raw key code by number: 0x45 */
            char *end;
            long v = strtol(low + 2, &end, 16);
            if (!*end && v >= 0 && v < 0x80)
                code = v;
        }
        if (code < 0 && low[0] && !low[1]) {
            static const char row1[] = "1234567890", r2[] = "qwertyuiop",
                              r3[] = "asdfghjkl", r4[] = "zxcvbnm";
            const char *f;
            if ((f = strchr(row1, low[0]))) code = 0x01 + (f - row1);
            else if ((f = strchr(r2, low[0]))) code = 0x10 + (f - r2);
            else if ((f = strchr(r3, low[0]))) code = 0x20 + (f - r3);
            else if ((f = strchr(r4, low[0]))) code = 0x31 + (f - r4);
        }
    }
    if (code < 0 || *n + (nm * 2 + 2) * 4 > cap)
        return FALSE;
#define RX_EV(c, q) do { out[(*n)++] = 0; out[(*n)++] = (UBYTE)(c); \
                         out[(*n)++] = (UBYTE)((q) >> 8); \
                         out[(*n)++] = (UBYTE)(q); } while (0)
    for (i = 0; i < nm; i++) {
        qual |= rx_mods[mods[i]].qual;
        RX_EV(rx_mods[mods[i]].code, qual);
    }
    RX_EV(code, qual);
    RX_EV(code | 0x80, qual);
    for (i = nm - 1; i >= 0; i--) {
        qual &= ~rx_mods[mods[i]].qual;
        RX_EV(rx_mods[mods[i]].code | 0x80, qual);
    }
#undef RX_EV
    return TRUE;
}

/* TYPE: text through the keymap, with <key> tokens; "<<" is a '<'. */
static LONG rx_type(const char *s)
{
    static UBYTE text[KEY_MAX_TEXT], ev[256];
    LONG tn = 0, en;
    const char *why;

    while (*s) {
        if (s[0] == '<' && s[1] == '<') {
            if (tn < KEY_MAX_TEXT) text[tn++] = '<';
            s += 2;
            continue;
        }
        if (*s == '<' && strchr(s, '>')) {
            char name[32];
            LONG nn = 0;
            if (tn && (why = do_key(KEY_TEXT, text, tn)))
                return rx_fail(10, why);
            tn = 0;
            for (s++; *s != '>' && nn < (LONG)sizeof(name) - 1; s++)
                name[nn++] = *s;
            name[nn] = '\0';
            while (*s && *s != '>')
                s++;
            if (*s)
                s++;
            en = 0;
            if (!rx_combo(name, ev, &en, sizeof(ev)))
                return rx_fail(10, "unknown key name in <...>");
            if ((why = do_key(KEY_RAW, ev, en)))
                return rx_fail(10, why);
            continue;
        }
        if (tn < KEY_MAX_TEXT)
            text[tn++] = (UBYTE)*s;
        s++;
    }
    if (tn && (why = do_key(KEY_TEXT, text, tn)))
        return rx_fail(10, why);
    return 0;
}

/* The window's options after the main argument: WINDOW pat, NTH n,
 * DOUBLE, RIGHT, MIDDLE. */
struct RxOpts {
    char window[128];
    BOOL has_window, dbl;
    LONG nth, button;
};

static void rx_opts(char *p, struct RxOpts *o)
{
    char w[128], low[16];
    memset(o, 0, sizeof(*o));
    while (rx_word(&p, w, sizeof(w))) {
        rx_norm(w, low, sizeof(low));
        if (strcmp(low, "window") == 0 && rx_word(&p, o->window,
                                                  sizeof(o->window)))
            o->has_window = TRUE;
        else if (strcmp(low, "nth") == 0 && rx_word(&p, w, sizeof(w)))
            o->nth = atol(w);
        else if (strcmp(low, "double") == 0)
            o->dbl = TRUE;
        else if (strcmp(low, "right") == 0)
            o->button = 1;
        else if (strcmp(low, "middle") == 0)
            o->button = 2;
    }
}

#define RX_MAX_ITEMS 128
static struct UiItem *g_rx_items;    /* 39 KB: taken on first use */
static LONG g_rx_n;

static BOOL rx_collect(struct UiItem *it, void *ctx)
{
    if (g_rx_items && g_rx_n < RX_MAX_ITEMS)
        g_rx_items[g_rx_n++] = *it;
    return TRUE;
}

/* The same list `wasabi ui` sees - Intuition, GadTools, ReAction, MUI -
 * matched like the PC matches: exact, then start, then part of a label;
 * a label of plain text is never the thing clicked. */
static LONG rx_clickgadget(const char *label, struct RxOpts *o)
{
    static char want[120], nlab[120];
    struct Window *w;
    struct UiItem *hit = NULL;
    LONG best = 3, count = 0, idx = 0, k = 1, i, wx = 0, wy = 0;
    BOOL active;
    const char *why;
    ULONG ib;

    rx_norm(label, want, sizeof(want));
    if (!g_rx_items &&
        !(g_rx_items = AllocMem(sizeof(struct UiItem) * RX_MAX_ITEMS,
                                MEMF_ANY)))
        return rx_fail(20, "out of memory");
    mui3_prescan();
    ib = LockIBase(0);
    w = rx_find_window(o->has_window ? o->window : NULL);
    if (!w) {
        UnlockIBase(ib);
        return rx_fail(5, "no such window");
    }
    g_rx_n = 0;
    ui_items(w, o->has_window, rx_collect, NULL);
    for (i = 0; i < g_rx_n; i++) {
        struct UiItem *it = &g_rx_items[i];
        int t;
        if (strcmp(it->kind, "label") == 0)
            continue;
        rx_norm(it->label[0] ? it->label : it->kind, nlab, sizeof(nlab));
        t = rx_tier(nlab, want);
        if (t >= 0 && t < best)
            best = t;
    }
    for (i = 0; i < g_rx_n && best < 3; i++) {
        struct UiItem *it = &g_rx_items[i];
        if (strcmp(it->kind, "label") == 0)
            continue;
        rx_norm(it->label[0] ? it->label : it->kind, nlab, sizeof(nlab));
        if (rx_tier(nlab, want) != best)
            continue;
        count++;
        if (++idx == (o->nth > 0 ? o->nth : 1))
            hit = it;
    }
    active = (w == IntuitionBase->ActiveWindow &&
              w->WScreen == IntuitionBase->FirstScreen);
    k = rx_row_scale(w->WScreen);
    wx = w->LeftEdge;
    wy = w->TopEdge;
    UnlockIBase(ib);

    if (!hit)
        return rx_fail(5, count ? "NTH is past the matches" : "no such gadget");
    if (count > 1 && o->nth <= 0)
        return rx_fail(10, "several gadgets match - add NTH n");
    if (hit->disabled)
        return rx_fail(10, "that gadget is disabled");
    if (!active) {
        if ((why = window_front(w)))
            return rx_fail(10, why);
        Delay(10);                       /* Intuition does it in its time */
    }
    if ((why = do_mouse(1, (ULONG)o->button, o->dbl ? 2 : 1,
                        (WORD)(wx + hit->x + hit->w / 2),
                        (WORD)((wy + hit->y + hit->h / 2) * k))))
        return rx_fail(10, why);
    return 0;
}

static LONG rx_menu(const char *path, struct RxOpts *o)
{
    static char want[200], seg[8][64], item[200], nitem[200];
    struct Window *w;
    struct Menu *m;
    struct MenuItem *it, *sub;
    LONG nseg = 0, best = 99, count = 0, mn, in, sn;
    ULONG code = 0;
    const char *p = path, *why;
    ULONG ib;

    while (*p && nseg < 8) {             /* "Project/Save As" -> segments */
        LONG n = 0;
        char raw[64];
        while (*p && *p != '/' && n < 63)
            raw[n++] = *p++;
        raw[n] = '\0';
        if (*p == '/')
            p++;
        rx_norm(raw, seg[nseg], sizeof(seg[0]));
        if (seg[nseg][0])
            nseg++;
    }
    if (!nseg)
        return rx_fail(10, "MENU needs a path, e.g. \"Project/Quit\"");
    (void)want;

    ib = LockIBase(0);
    w = rx_find_window(o->has_window ? o->window : NULL);
    if (!w || !w->MenuStrip) {
        UnlockIBase(ib);
        return rx_fail(5, w ? "that window has no menus" : "no such window");
    }
    /* Score each pickable item: the sum of its segments' tiers, the
     * path compared from the end when fewer segments were given. */
    for (m = w->MenuStrip, mn = 0; m && mn < 31; m = m->NextMenu, mn++)
        for (it = m->FirstItem, in = 0; it && in < 63;
             it = it->NextItem, in++)
            for (sub = it->SubItem, sn = 0; ; sub = sub->NextItem, sn++) {
                struct MenuItem *x = it->SubItem ? sub : it;
                char parts[3][64];
                LONG np = 0, n, score = 0, j;
                if (it->SubItem && !sub)
                    break;
                n = 0; parts[np][0] = '\0';
                ui_put(parts[np], &n, 64, (UBYTE *)m->MenuName);
                np++;
                n = 0; parts[np][0] = '\0';
                ui_item_text(it, parts[np], &n, 64);
                np++;
                if (it->SubItem) {
                    n = 0; parts[np][0] = '\0';
                    ui_item_text(sub, parts[np], &n, 64);
                    np++;
                }
                if (nseg > np || !(x->Flags & ITEMTEXT))
                    goto next;
                for (j = 0; j < nseg; j++) {
                    int t;
                    rx_norm(parts[np - nseg + j], nitem, sizeof(nitem));
                    t = rx_tier(nitem, seg[j]);
                    if (t < 0)
                        goto next;
                    score += t;
                }
                if (score < best) {
                    best = score;
                    count = 0;
                }
                if (score == best) {
                    count++;
                    code = mn | (in << 5) |
                           ((it->SubItem ? (ULONG)sn : 31UL) << 11);
                }
            next:
                if (!it->SubItem || sn >= 30)
                    break;
            }
    UnlockIBase(ib);
    (void)item;

    if (!count)
        return rx_fail(5, "no such menu item");
    if (count > 1)
        return rx_fail(10, "several menu items match - give more of the path");
    if ((why = menu_pick(w, code)))
        return rx_fail(10, why);
    return 0;
}

/* One command. rc as ARexx has it; res gets the RESULT text. */
static LONG rx_command(char *line, char *res, LONG ressz)
{
    static char cmd[32], arg[KEY_MAX_TEXT + 1];
    char lowcmd[32];
    struct RxOpts o;
    char *p = line;
    ULONG ib;

    res[0] = '\0';
    if (!rx_word(&p, cmd, sizeof(cmd)))
        return rx_fail(20, "empty command");
    rx_norm(cmd, lowcmd, sizeof(lowcmd));

    if (strcmp(lowcmd, "version") == 0) {
        copystr(res, ressz, VERSION_STR);
        return 0;
    }
    if (strcmp(lowcmd, "lasterror") == 0) {
        copystr(res, ressz, g_rx_error[0] ? g_rx_error : "no error");
        return 0;
    }
    g_rx_error[0] = '\0';               /* every other command starts clean */
    if (strcmp(lowcmd, "help") == 0) {
        copystr(res, ressz, "VERSION WINDOWS ACTIVATEWINDOW TYPE KEY CLICK "
                "CLICKGADGET MENU LASTERROR HELP");
        return 0;
    }
    if (strcmp(lowcmd, "windows") == 0) {
        struct Screen *sc;
        struct Window *w;
        LONG n = 0;
        ib = LockIBase(0);
        for (sc = IntuitionBase->FirstScreen; sc; sc = sc->NextScreen)
            for (w = sc->FirstWindow; w; w = w->NextWindow)
                if (w->Title && n < ressz - 2) {
                    ui_put(res, &n, ressz - 1, (UBYTE *)w->Title);
                    res[n++] = '\n';
                    res[n] = '\0';
                }
        UnlockIBase(ib);
        if (n)
            res[n - 1] = '\0';
        return 0;
    }
    if (strcmp(lowcmd, "activatewindow") == 0) {
        struct Window *w;
        const char *why;
        if (!rx_word(&p, arg, sizeof(arg)))
            return rx_fail(10, "ACTIVATEWINDOW needs a title pattern");
        ib = LockIBase(0);
        w = rx_find_window(arg);
        if (w)
            copystr(res, ressz, w->Title ? (char *)w->Title : "");
        UnlockIBase(ib);
        if (!w)
            return rx_fail(5, "no window matches");
        if ((why = window_front(w)))
            return rx_fail(10, why);
        return 0;
    }
    if (strcmp(lowcmd, "type") == 0 || strcmp(lowcmd, "entertext") == 0) {
        while (*p == ' ')
            p++;
        if (*p == '"' || *p == '\'') {
            if (!rx_word(&p, arg, sizeof(arg)))
                arg[0] = '\0';
        } else
            copystr(arg, sizeof(arg), p);
        return rx_type(arg);
    }
    if (strcmp(lowcmd, "key") == 0) {
        static UBYTE ev[256];
        LONG en = 0;
        const char *why;
        while (rx_word(&p, arg, sizeof(arg)))
            if (!rx_combo(arg, ev, &en, sizeof(ev)))
                return rx_fail(10, "unknown key name");
        if (!en)
            return rx_fail(10, "KEY needs a key name, e.g. return");
        if ((why = do_key(KEY_RAW, ev, en)))
            return rx_fail(10, why);
        return 0;
    }
    if (strcmp(lowcmd, "click") == 0) {
        char xs[16], ys[16];
        LONG k;
        const char *why;
        if (!rx_word(&p, xs, sizeof(xs)) || !rx_word(&p, ys, sizeof(ys)))
            return rx_fail(10, "CLICK needs X and Y on the front screen");
        rx_opts(p, &o);
        ib = LockIBase(0);
        k = rx_row_scale(IntuitionBase->FirstScreen);
        UnlockIBase(ib);
        if ((why = do_mouse(1, (ULONG)o.button, o.dbl ? 2 : 1,
                            (WORD)atol(xs), (WORD)(atol(ys) * k))))
            return rx_fail(10, why);
        return 0;
    }
    if (strcmp(lowcmd, "clickgadget") == 0) {
        if (!rx_word(&p, arg, sizeof(arg)))
            return rx_fail(10, "CLICKGADGET needs a label");
        rx_opts(p, &o);
        return rx_clickgadget(arg, &o);
    }
    if (strcmp(lowcmd, "menu") == 0) {
        if (!rx_word(&p, arg, sizeof(arg)))
            return rx_fail(10, "MENU needs a path");
        rx_opts(p, &o);
        return rx_menu(arg, &o);
    }
    return rx_fail(20, "not a WASABI command - HELP lists them");
}

static void rexxport_pump(void)
{
    static char line[KEY_MAX_TEXT + 64], res[4096];
    struct RexxMsg *rm;

    if (!g_rexx_port)
        return;
    while ((rm = (struct RexxMsg *)GetMsg(g_rexx_port))) {
        LONG rc;
        rm->rm_Result1 = 20;
        rm->rm_Result2 = 0;
        if (rm->rm_Args[0]) {
            copystr(line, sizeof(line), (const char *)rm->rm_Args[0]);
            rc = rx_command(line, res, sizeof(res));
            rm->rm_Result1 = rc;
            if (rc == 0 && (rm->rm_Action & RXFF_RESULT) && res[0])
                rm->rm_Result2 = (LONG)CreateArgstring(res,
                                                       (ULONG)strlen(res));
        }
        ReplyMsg((struct Message *)rm);
    }
}

/*
 * Throughput measurement, deliberately storage-free: received bytes are
 * counted and dropped, sent bytes come from one static-pattern buffer.
 * Nothing lands in RAM: or on a volume, so a stock 2 MB machine runs
 * the same 50 MB test a PiStorm does, and the number isolates the
 * network path instead of blending in a filesystem.
 */
/*
 * Grab a screen and send it as raw RGB.
 *
 * Raw, not compressed, and that is the whole design. Everything on this
 * machine is slower than its network: the wire does 109 MB/s while the
 * disk manages 20 and the CPU is the bottleneck behind every slow thing
 * we have measured. A 1280x960 screen is 3.5 MB, which is thirty
 * milliseconds of wire - far less than the Amiga would spend deflating
 * it. So the Amiga reads pixels and nothing else; the Linux box, which
 * is idle and fast, turns them into a PNG.
 *
 * Two ways to read those pixels, chosen by the screen's depth, because
 * neither works everywhere:
 *
 *   > 8 bits   CyberGraphX ReadPixelArray(), true RGB straight out.
 *              Its autodoc says plainly "should only be used on screens
 *              depths > 8 bits", so it is not an option below that.
 *
 *   <= 8 bits  graphics.library ReadPixelArray8() gives pen numbers,
 *              and GetRGB32() turns the screen's palette into the RGB
 *              they stand for. Plain OS 3.x - it needs no RTG stack at
 *              all, so this path also covers a stock machine with no
 *              Picasso96, and every native PAL/NTSC screen.
 *
 * The stream is a 12-byte header (width, height, bytes per pixel) then
 * the rows, in DATA frames, then END - the same either way, so the
 * client never learns which path was taken.
 */
#define SCREEN_ALIGN(w) (((w) + 15) & ~15)   /* ReadPixelArray8 wants this */

static BOOL screen_send_deep(int fd, struct Screen *sc, ULONG w, ULONG h)
{
    UBYTE *buf;
    LONG rowbytes = (LONG)w * 3, band, y;
    BOOL ok = TRUE;

    if (!g_cgfx)
        g_cgfx = OpenLibrary("cybergraphics.library", 40);
    if (!g_cgfx)
        return send_perr(fd, "that screen is deeper than 8 bits but there is "
                            "no cybergraphics.library to read it with");
    band = MAX_PAYLOAD / rowbytes;
    if (band < 1) band = 1;
    buf = AllocMem(band * rowbytes, MEMF_ANY);
    if (!buf)
        return send_perr(fd, "out of memory for the grab buffer");

    for (y = 0; ok && y < (LONG)h; y += band) {
        LONG n = (y + band > (LONG)h) ? ((LONG)h - y) : band;
        __ReadPixelArray_base(g_cgfx, buf, 0, 0, (UWORD)rowbytes,
                              &sc->RastPort, 0, (UWORD)y,
                              (UWORD)w, (UWORD)n, RECTFMT_RGB);
        if (!send_frame(fd, T_DATA, buf, n * rowbytes))
            ok = FALSE;
    }
    FreeMem(buf, band * rowbytes);
    return ok;
}

/*
 * The one-row scratch bitmap ReadPixelArray8 converts through. It must
 * have the SCREEN's depth and be cleared: it was 8 planes and not
 * cleared, so on a 4-plane screen the blit filled planes 0-3 and the
 * conversion read planes 4-7 as well - leftover memory, the same at
 * every column of that one row - and every row of a planar grab or live
 * view got the same wrong pens: white/grey vertical stripes at fixed x
 * (FS-UAE's 16-colour Workbench, 2026-10-04 and -10). RTG screens take
 * the CyberGraphX road and never came here.
 */
static struct BitMap *scratch_row(struct Screen *sc, LONG aligned)
{
    ULONG depth = bm_depth(sc->RastPort.BitMap);
    struct BitMap *bm;
    LONG i;
    if (depth < 1 || depth > 8)
        depth = 8;
    if (gfx39())
        return AllocBitMap(aligned, 1, depth, BMF_CLEAR, NULL);
    /* OS 2.x: no AllocBitMap - build one row by hand, in chip RAM. */
    if (!(bm = AllocMem(sizeof(*bm), MEMF_CLEAR)))
        return NULL;
    InitBitMap(bm, (BYTE)depth, (UWORD)aligned, 1);
    for (i = 0; i < (LONG)depth; i++)
        if (!(bm->Planes[i] = (PLANEPTR)AllocRaster(aligned, 1))) {
            scratch_free(bm);
            return NULL;
        }
    return bm;
}

static BOOL screen_send_planar(int fd, struct Screen *sc, ULONG w, ULONG h,
                               LONG depth)
{
    struct RastPort temprp;
    /* 3 KB, and at -O2 every cmd_* is inlined into serve(), so an
     * automatic here is charged to EVERY command the daemon serves -
     * ping included - not just to a screen grab. Static costs 3 KB of
     * BSS once and takes it off the stack budget for good. */
    static ULONG pal[256 * 3];
    UBYTE *pens, *rgb;
    LONG aligned = SCREEN_ALIGN(w), rowbytes = (LONG)w * 3;
    LONG band, y, ncol = 1L << depth;
    BOOL ok = TRUE;

    if (ncol > 256) ncol = 256;
    band = MAX_PAYLOAD / rowbytes;
    if (band < 1) band = 1;

    /* ReadPixelArray8 needs a scratch RastPort one row deep, and it must
     * not have a Layer or it would clip against the real window. */
    temprp = sc->RastPort;
    temprp.Layer = NULL;
    temprp.BitMap = scratch_row(sc, aligned);
    if (!temprp.BitMap)
        return send_perr(fd, "out of memory for the scratch bitmap");

    pens = AllocMem(aligned * band, MEMF_ANY);
    rgb  = AllocMem(rowbytes * band, MEMF_ANY);
    if (!pens || !rgb) {
        if (pens) FreeMem(pens, aligned * band);
        if (rgb)  FreeMem(rgb, rowbytes * band);
        scratch_free(temprp.BitMap);
        return send_perr(fd, "out of memory for the grab buffer");
    }

    /* GetRGB32 hands back 32-bit components; we want the top byte. */
    get_rgb32(sc->ViewPort.ColorMap, (ULONG)ncol, pal);

    for (y = 0; ok && y < (LONG)h; y += band) {
        LONG n = (y + band > (LONG)h) ? ((LONG)h - y) : band;
        LONG row, col;
        ReadPixelArray8(&sc->RastPort, 0, (ULONG)y, w - 1,
                        (ULONG)(y + n - 1), pens, &temprp);
        for (row = 0; row < n; row++) {
            UBYTE *src = pens + row * aligned;
            UBYTE *dst = rgb + row * rowbytes;
            for (col = 0; col < (LONG)w; col++) {
                LONG pen = src[col];
                if (pen >= ncol) pen = 0;
                *dst++ = (UBYTE)(pal[pen * 3]     >> 24);
                *dst++ = (UBYTE)(pal[pen * 3 + 1] >> 24);
                *dst++ = (UBYTE)(pal[pen * 3 + 2] >> 24);
            }
        }
        if (!send_frame(fd, T_DATA, rgb, n * rowbytes))
            ok = FALSE;
    }
    FreeMem(pens, aligned * band);
    FreeMem(rgb, rowbytes * band);
    scratch_free(temprp.BitMap);
    return ok;
}


/* --- LIVE: the screen for a live view - only what changed, compact -- */

/*
 * GRAB sends every pixel as 24-bit RGB, which is right for a picture to
 * keep and wrong for a live view: 3.7 MB a frame of the A1200's
 * Workbench, over a Wi-Fi hop doing ~28 MB/s, is the whole frame budget.
 * LIVE keeps, per client, the last picture that client was sent, and
 * sends only the rectangles that differ from it, in a compact format:
 *
 *   deep screens (> 8 bits)  RGB565, 2 bytes a pixel (lossy for 24-bit,
 *                            fine for watching; GRAB stays exact)
 *   8 bits or fewer          the pen numbers, 1 byte a pixel, plus the
 *                            palette whenever it changes - exact
 *
 * Every DATA frame is one self-describing message (the bridge forwards
 * them to the page untouched):
 *
 *   0x10 'head'   u16 w, u16 h, u8 format (1 = RGB565, 2 = pen8),
 *                 u8 full (1 = the page must drop what it has)
 *   0x11 'pal'    u16 n, then n * RGB bytes
 *   0x12 'rect'   u16 x, u16 y, u16 w, u16 h, then w*h pixels, row by row
 *
 * then END. A frame where nothing changed is the head and the END.
 * flags bit 0 asks for everything (a page that has just connected).
 * The comparison is per row - first and last changed pixel - with
 * neighbouring changed rows merged into one rectangle while it stays
 * under one frame's worth of bytes.
 */
#define LIVE_RGB565 1
#define LIVE_PEN8   2
#define LIVE_RECT_MAX (MAX_PAYLOAD - 16)

struct LiveState {
    struct Screen *sc;                  /* what prev is a picture of */
    UWORD w, h, fmt;
    ULONG size;                         /* bytes in prev */
    UBYTE *prev;                        /* the client's picture, w*h*bpp */
    UBYTE *raw;                         /* deep screens read in place: the
                                         * card's own pixels, last frame */
    ULONG rawsize, rawpf, rawbpr;
    ULONG pal[256];                     /* pen8: 0x00RRGGBB as last sent */
    UWORD npal;
};
static struct LiveState *g_live[MAX_CLIENTS];

/* A row of an RTG screen for the live view, 64 KB: taken the first time
 * a live view needs it, freed at exit. */
static ULONG *g_rowbuf;

static ULONG *live_rowbuf(void)
{
    if (!g_rowbuf)
        g_rowbuf = AllocMem(LIVE_RECT_MAX, MEMF_ANY);
    return g_rowbuf;
}

static void live_free(int cl)
{
    struct LiveState *ls = g_live[cl];
    if (!ls)
        return;
    if (ls->prev)
        FreeMem(ls->prev, ls->size);
    if (ls->raw)
        FreeMem(ls->raw, ls->rawsize);
    FreeMem(ls, sizeof(*ls));
    g_live[cl] = NULL;
}

/* One rectangle out of prev (which already holds the new pixels). */
static BOOL live_send_rect(int fd, struct LiveState *ls, UBYTE *out,
                           LONG x0, LONG x1, LONG y0, LONG y1)
{
    LONG bpp = ls->fmt == LIVE_RGB565 ? 2 : 1;
    LONG rw = x1 - x0, y, n = 9;
    out[0] = 0x12;
    out[1] = x0 >> 8; out[2] = x0;
    out[3] = y0 >> 8; out[4] = y0;
    out[5] = rw >> 8; out[6] = rw;
    out[7] = (y1 - y0) >> 8; out[8] = (y1 - y0);
    for (y = y0; y < y1; y++) {
        memcpy(out + n, ls->prev + ((ULONG)y * ls->w + x0) * bpp, rw * bpp);
        n += rw * bpp;
    }
    return send_frame(fd, T_DATA, out, n);
}

/*
 * The changed rectangles of one frame, found in the first pass (with
 * the screen's memory locked) and sent in the second (unlocked): the
 * network must never hold the Amiga's drawing still. Past LIVE_MAXRECT
 * the rest of the screen goes as rectangles of whole rows.
 */
#define LIVE_MAXRECT 512
static struct { UWORD x0, x1, y0, y1; } g_rects[LIVE_MAXRECT];
static LONG g_nrects;
static LONG g_overflow_y;               /* -1, or where the list ran out */

struct LiveScan {                       /* the rectangle being built */
    LONG ry0, rx0, rx1, bpp, w;
};

static void live_close(struct LiveScan *sc, LONG yend)
{
    if (sc->ry0 < 0)
        return;
    if (g_nrects < LIVE_MAXRECT) {
        g_rects[g_nrects].x0 = sc->rx0;
        g_rects[g_nrects].x1 = sc->rx1;
        g_rects[g_nrects].y0 = sc->ry0;
        g_rects[g_nrects].y1 = yend;
        g_nrects++;
    } else if (g_overflow_y < 0) {
        /* List full: everything from here down goes as whole-width
         * bands, each sized to fit one frame (live_send_bands). This
         * used to widen the last rectangle to the bottom instead - a
         * rectangle far bigger than the 64 KB send buffer, and the copy
         * into it overran the heap (audit 4: reachable on a screen over
         * 1024 lines tall with alternating changed rows). */
        g_overflow_y = sc->ry0;
    }
    sc->ry0 = -1;
}

/* Row yy changed between columns first..last (first < 0: unchanged). */
static void live_row(struct LiveScan *sc, LONG yy, LONG first, LONG last)
{
    if (first < 0) {
        live_close(sc, yy);
        return;
    }
    if (sc->ry0 >= 0) {
        LONG nx0 = first < sc->rx0 ? first : sc->rx0;
        LONG nx1 = last + 1 > sc->rx1 ? last + 1 : sc->rx1;
        if ((yy + 1 - sc->ry0) * (nx1 - nx0) * sc->bpp > LIVE_RECT_MAX) {
            live_close(sc, yy);
        } else {
            sc->rx0 = nx0;
            sc->rx1 = nx1;
            return;
        }
    }
    sc->ry0 = yy;
    sc->rx0 = first;
    sc->rx1 = last + 1;
}

/*
 * Compare one row of new RGB565 pixels, produced by EXPR from the
 * source pointer s (advanced by STEP bytes a pixel), against prev.
 */
#define LIVE_DIFF565(STEP, EXPR) do { \
        for (x = 0; x < (LONG)w; x++, s += (STEP)) { \
            UWORD px = (EXPR); \
            if (full || pv[x] != px) { \
                if (first < 0) first = x; \
                last = x; \
                pv[x] = px; \
            } \
        } \
    } while (0)

#define C565(r, g, b) (UWORD)(((((r) & 0xFF) & 0xF8) << 8) | \
                              ((((g) & 0xFF) & 0xFC) << 3) | \
                              (((b) & 0xFF) >> 3))

/* One row from a direct (locked) framebuffer in pixel format pf.
 * FALSE for a format this does not know - the caller falls back.
 *
 * Graphics-card memory is slow to read from the 68k side under Emu68
 * (measured on the A1200, 1280x960: one longword a pixel takes 29 ms
 * for the screen, the same loop over fast RAM 5 ms) - so each pixel is
 * ONE read of its full width, and the colour is taken apart in a
 * register. Reading it a byte at a time cost three trips and 213 ms. */
static BOOL live_row565(ULONG pf, const UBYTE *row, UWORD *pv, ULONG w,
                        BOOL full, LONG *pfirst, LONG *plast)
{
    LONG x, first = -1, last = -1;
    const ULONG *l = (const ULONG *)row;
    const UWORD *s16 = (const UWORD *)row;
    const UBYTE *s = row;
    ULONG v;
    switch (pf) {
    case PIXFMT_ARGB32:                 /* bytes A R G B */
        for (x = 0; x < (LONG)w; x++) {
            UWORD px;
            v = l[x];
            px = C565(v >> 16, v >> 8, v);
            if (full || pv[x] != px) {
                if (first < 0) first = x;
                last = x;
                pv[x] = px;
            }
        }
        break;
    case PIXFMT_BGRA32:                 /* bytes B G R A */
        for (x = 0; x < (LONG)w; x++) {
            UWORD px;
            v = l[x];
            px = C565(v >> 8, v >> 16, v >> 24);
            if (full || pv[x] != px) {
                if (first < 0) first = x;
                last = x;
                pv[x] = px;
            }
        }
        break;
    case PIXFMT_RGBA32:                 /* bytes R G B A */
        for (x = 0; x < (LONG)w; x++) {
            UWORD px;
            v = l[x];
            px = C565(v >> 24, v >> 16, v >> 8);
            if (full || pv[x] != px) {
                if (first < 0) first = x;
                last = x;
                pv[x] = px;
            }
        }
        break;
    case PIXFMT_RGB16:                  /* already RGB565, big-endian */
        for (x = 0; x < (LONG)w; x++) {
            UWORD px = s16[x];
            if (full || pv[x] != px) {
                if (first < 0) first = x;
                last = x;
                pv[x] = px;
            }
        }
        break;
    case PIXFMT_RGB16PC:                /* RGB565, little-endian */
        for (x = 0; x < (LONG)w; x++) {
            UWORD px = s16[x];
            px = (UWORD)((px << 8) | (px >> 8));
            if (full || pv[x] != px) {
                if (first < 0) first = x;
                last = x;
                pv[x] = px;
            }
        }
        break;
    case PIXFMT_RGB24: LIVE_DIFF565(3, C565(s[0], s[1], s[2])); break;
    case PIXFMT_BGR24: LIVE_DIFF565(3, C565(s[2], s[1], s[0])); break;
    default:
        return FALSE;
    }
    *pfirst = first;
    *plast = last;
    return TRUE;
}

static BOOL cmd_live(int cl, int fd, ULONG flags)
{
    struct LiveState *ls;
    struct Screen *sc;
    struct RastPort temprp;
    struct LiveScan scan;
    ULONG ib, w, h, pf = 0, bpr = 0;
    LONG depth, fmt, bpp, band, y, i, rowbytes = 0, aligned;
    UBYTE *src = NULL, *out = NULL, *base = NULL, head[8];
    LONG srcsize = 0;
    APTR lock = NULL;
    BOOL full, ok = TRUE, nomem = FALSE;
    ULONG t0 = health_usecs(), t1 = 0;
    UBYTE path = 0;                     /* 1 direct, 2 ReadPixelArray */

    ib = LockIBase(0);
    sc = IntuitionBase->FirstScreen;
    UnlockIBase(ib);
    if (!sc)
        return send_perr(fd, "there are no screens open");
    w = (ULONG)sc->Width;
    h = (ULONG)sc->Height;
    depth = bm_depth(sc->RastPort.BitMap);
    fmt = depth > 8 ? LIVE_RGB565 : LIVE_PEN8;
    bpp = fmt == LIVE_RGB565 ? 2 : 1;
    if (!w || !h || (LONG)w * 4 > LIVE_RECT_MAX)
        return send_perr(fd, "that screen is too wide for a live view");
    if (fmt == LIVE_RGB565 && !g_cgfx)
        g_cgfx = OpenLibrary("cybergraphics.library", 40);
    if (fmt == LIVE_RGB565 && !g_cgfx)
        return send_perr(fd, "a deep screen and no cybergraphics.library");

    ls = g_live[cl];
    if (!ls) {
        ls = AllocMem(sizeof(*ls), MEMF_ANY | MEMF_CLEAR);
        if (!ls)
            return send_err(fd, "out of memory");
        g_live[cl] = ls;
    }
    /* Why a full frame, for the page's (and a debugger's) benefit. */
    head[7] = ((flags & 1) ? 1 : 0) | (ls->sc != sc ? 2 : 0) |
              (ls->w != w ? 4 : 0) | (ls->h != h ? 8 : 0) |
              (ls->fmt != fmt ? 16 : 0) | (!ls->prev ? 32 : 0);
    full = head[7] != 0;
    if (full) {
        if (ls->prev)
            FreeMem(ls->prev, ls->size);
        ls->size = w * h * bpp;
        ls->prev = AllocMem(ls->size, MEMF_ANY);
        ls->npal = 0;
        if (!ls->prev) {
            live_free(cl);
            return send_err(fd, "out of memory for the live picture");
        }
        ls->sc = sc; ls->w = w; ls->h = h; ls->fmt = fmt;
    }

    head[0] = 0x10;
    head[1] = w >> 8; head[2] = w;
    head[3] = h >> 8; head[4] = h;
    head[5] = fmt;
    head[6] = full;
    if (!send_frame(fd, T_DATA, head, 8))
        return FALSE;

    out = AllocMem(LIVE_RECT_MAX + 16, MEMF_ANY);
    if (!out) {
        live_free(cl);
        return send_err(fd, "out of memory for the live view");
    }
    g_nrects = 0;
    g_overflow_y = -1;
    scan.ry0 = -1;
    scan.bpp = bpp;
    scan.w = w;
    temprp.BitMap = NULL;

    if (fmt == LIVE_PEN8) {
        /* The palette first: a changed palette changes every pixel's
         * colour without changing a single pen number. */
        static ULONG rgb32[256 * 3];
        LONG ncol = 1L << depth, changed = full;
        if (ncol > 256) ncol = 256;
        get_rgb32(sc->ViewPort.ColorMap, (ULONG)ncol, rgb32);
        for (i = 0; i < ncol; i++) {
            ULONG c = ((rgb32[i * 3] >> 24) << 16) |
                      ((rgb32[i * 3 + 1] >> 24) << 8) |
                      (rgb32[i * 3 + 2] >> 24);
            if (i >= ls->npal || ls->pal[i] != c) changed = TRUE;
            ls->pal[i] = c;
        }
        ls->npal = ncol;
        if (changed) {
            out[0] = 0x11; out[1] = ncol >> 8; out[2] = ncol;
            for (i = 0; i < ncol; i++) {
                out[3 + i * 3] = ls->pal[i] >> 16;
                out[4 + i * 3] = ls->pal[i] >> 8;
                out[5 + i * 3] = ls->pal[i];
            }
            ok = send_frame(fd, T_DATA, out, 3 + ncol * 3);
        }
    } else {
        /*
         * A graphics-card screen: lock its memory and read it in place.
         * ReadPixelArray converted every pixel to 24-bit RGB first and
         * cost ~95 ms a frame on the A1200 for a screen nothing had
         * touched. The lock is held only for the comparison - no
         * network traffic happens under it.
         */
        struct TagItem tags[4];
        tags[0].ti_Tag = LBMI_BASEADDRESS; tags[0].ti_Data = (ULONG)&base;
        tags[1].ti_Tag = LBMI_BYTESPERROW; tags[1].ti_Data = (ULONG)&bpr;
        tags[2].ti_Tag = LBMI_PIXFMT;      tags[2].ti_Data = (ULONG)&pf;
        tags[3].ti_Tag = TAG_DONE;         tags[3].ti_Data = 0;
        /* A row at a time is copied out with CopyMem first and compared
         * in fast RAM: a loop that read the card's memory and fast RAM
         * by turns took 200 ms a frame on the A1200, a bulk copy and a
         * fast-RAM loop a fraction of that (measured). */
        ULONG *rowbuf = live_rowbuf();      /* longword aligned */
        lock = rowbuf ? __LockBitMapTagList_base(g_cgfx, sc->RastPort.BitMap,
                                                 tags) : NULL;
        if (lock && base && w * 4 <= LIVE_RECT_MAX) {
            ULONG pbytes = (pf == PIXFMT_ARGB32 || pf == PIXFMT_BGRA32 ||
                            pf == PIXFMT_RGBA32) ? 4 :
                           (pf == PIXFMT_RGB16 || pf == PIXFMT_RGB16PC) ? 2 : 0;
            ULONG n = (w * 4 <= bpr ? w * 4 : bpr) & ~3UL;
            BOOL quick = !(((ULONG)base | bpr) & 3);
            /*
             * 4- and 2-byte formats: keep the card's own pixels from last
             * time and compare those a longword at a time - no colour
             * maths for a pixel that did not change; only the changed
             * span is converted to RGB565.
             */
            /* An odd-width 16-bit screen ends in half a longword, which
             * the longword compare would never look at: let it take the
             * per-pixel road, which sees every pixel. */
            if (pbytes == 2 && (w & 1))
                pbytes = 0;
            if (pbytes) {
                ULONG need = w * h * pbytes;
                if (full || !ls->raw || ls->rawpf != pf || ls->rawsize != need) {
                    if (ls->raw)
                        FreeMem(ls->raw, ls->rawsize);
                    ls->raw = AllocMem(need, MEMF_ANY);
                    ls->rawsize = ls->raw ? need : 0;
                    ls->rawpf = pf;
                    full = TRUE;        /* nothing to compare against */
                }
                if (ls->raw)
                    n = w * pbytes;
                else
                    pbytes = 0;         /* no memory: per-pixel road, with
                                         * the row length set above - not
                                         * 0, which copied nothing and
                                         * compared stale data */
            }
            for (y = 0; y < (LONG)h; y++) {
                LONG first, last;
                if (quick && !(n & 3))
                    CopyMemQuick(base + (ULONG)y * bpr, rowbuf, n);
                else
                    CopyMem(base + (ULONG)y * bpr, rowbuf, n);
                if (pbytes) {
                    ULONG *nw = rowbuf, *ow = (ULONG *)(ls->raw + (ULONG)y * n);
                    LONG nwords = n / 4, a = 0, b = nwords - 1;
                    if (!full) {
                        while (a < nwords && nw[a] == ow[a]) a++;
                        if (a < nwords)
                            while (nw[b] == ow[b]) b--;
                    }
                    if (a >= nwords) {
                        first = last = -1;
                    } else {
                        LONG ignore0, ignore1;
                        CopyMem(&nw[a], &ow[a], (b - a + 1) * 4);
                        first = a * 4 / pbytes;
                        last = ((b + 1) * 4) / pbytes - 1;
                        /* convert just that span into the RGB565 copy */
                        live_row565(pf, (UBYTE *)rowbuf + first * pbytes,
                                    (UWORD *)(ls->prev + (ULONG)y * w * 2) + first,
                                    last - first + 1, TRUE, &ignore0, &ignore1);
                    }
                } else if (!live_row565(pf, (UBYTE *)rowbuf,
                                        (UWORD *)(ls->prev + (ULONG)y * w * 2),
                                        w, full, &first, &last))
                    break;                  /* unknown format: fall back */
                live_row(&scan, y, first, last);
            }
            __UnLockBitMap_base(g_cgfx, lock);
            if (y >= (LONG)h) {
                live_close(&scan, h);
                path = 1;
                goto send;
            }
            /* fell back part-way: start the comparison again cleanly */
            g_nrects = 0;
            g_overflow_y = -1;
            scan.ry0 = -1;
        } else if (lock) {
            __UnLockBitMap_base(g_cgfx, lock);
        }
    }

    /* The general road: ReadPixelArray(8) a band at a time. */
    aligned = SCREEN_ALIGN(w);
    rowbytes = fmt == LIVE_RGB565 ? (LONG)w * 3 : aligned;
    band = 32;
    srcsize = rowbytes * band;
    src = AllocMem(srcsize, MEMF_ANY);
    if (src && fmt == LIVE_PEN8) {
        temprp = sc->RastPort;
        temprp.Layer = NULL;
        temprp.BitMap = scratch_row(sc, aligned);
    }
    if (!src || (fmt == LIVE_PEN8 && !temprp.BitMap)) {
        ok = FALSE;
        nomem = TRUE;
        goto done;
    }
    for (y = 0; ok && y < (LONG)h; y += band) {
        LONG n = (y + band > (LONG)h) ? ((LONG)h - y) : band, r;
        if (fmt == LIVE_RGB565)
            __ReadPixelArray_base(g_cgfx, src, 0, 0, (UWORD)rowbytes,
                                  &sc->RastPort, 0, (UWORD)y,
                                  (UWORD)w, (UWORD)n, RECTFMT_RGB);
        else
            ReadPixelArray8(&sc->RastPort, 0, (ULONG)y, w - 1,
                            (ULONG)(y + n - 1), src, &temprp);
        for (r = 0; r < n; r++) {
            LONG yy = y + r, first = -1, last = -1, x;
            const UBYTE *s = src + r * rowbytes;
            if (fmt == LIVE_RGB565) {
                UWORD *pv = (UWORD *)(ls->prev + (ULONG)yy * w * 2);
                LIVE_DIFF565(3, C565(s[0], s[1], s[2]));
            } else {
                UBYTE *pv = ls->prev + (ULONG)yy * w;
                for (x = 0; x < (LONG)w; x++) {
                    if (full || pv[x] != s[x]) {
                        if (first < 0) first = x;
                        last = x;
                        pv[x] = s[x];
                    }
                }
            }
            live_row(&scan, yy, first, last);
        }
    }
    live_close(&scan, h);
    path = 2;

send:
    t1 = health_usecs();
    if (flags & 2) {                    /* asked how long it took */
        out[0] = 0x13; out[1] = path;
        put_be32(out + 2, t1 - t0);
        put_be32(out + 6, (ULONG)g_nrects);
        ok = send_frame(fd, T_DATA, out, 10);
    }
    for (i = 0; ok && i < g_nrects; i++)
        ok = live_send_rect(fd, ls, out, g_rects[i].x0, g_rects[i].x1,
                            g_rects[i].y0, g_rects[i].y1);
    if (ok && g_overflow_y >= 0) {      /* the rest, in bands that fit */
        LONG rows = LIVE_RECT_MAX / ((LONG)w * bpp);
        if (rows < 1) rows = 1;
        for (y = g_overflow_y; ok && y < (LONG)h; y += rows)
            ok = live_send_rect(fd, ls, out, 0, w, y,
                                y + rows < (LONG)h ? y + rows : (LONG)h);
    }
done:
    if (src) FreeMem(src, srcsize);
    FreeMem(out, LIVE_RECT_MAX + 16);
    if (temprp.BitMap)
        scratch_free(temprp.BitMap);
    if (!ok) {
        live_free(cl);                   /* half-sent: start over next time */
        /* out of memory: say so in END's place; a failed send has
         * already lost the connection */
        return nomem ? send_err(fd, "out of memory for the live view")
                     : FALSE;
    }
    return send_frame(fd, T_END, NULL, 0);
}

/*
 * List the open screens, front first, and optionally reorder them.
 *
 * Cycling is ScreenToBack() on the frontmost, which is precisely what
 * Amiga+M does - so there is no need to synthesise keystrokes into
 * input.device to flip screens from another machine, and naming one
 * beats flipping blindly through them.
 *
 * The whole walk runs under LockIBase() because the screen list is
 * Intuition's own and may change under a reader.
 */
static BOOL cmd_screenctl(int fd, ULONG flags, const char *want)
{
    struct Screen *sc, *bring = NULL, *front = NULL;
    char line[220];
    ULONG ib;
    LONG n;

    ib = LockIBase(0);
    for (sc = IntuitionBase->FirstScreen; sc; sc = sc->NextScreen) {
        LONG depth = bm_depth(sc->RastPort.BitMap);
        const char *title = sc->Title ? (const char *)sc->Title : "";
        if (!front)
            front = sc;
        if (want[0] && !bring && str_ieq(title, want))
            bring = sc;
        /* The title belongs to whichever program opened the screen -
         * some put a full path in it - so it is bounded here rather
         * than trusted into a 220-byte line. */
        n = sprintf(line, "0x%08lx %ld %ld %ld %.150s\n",
                    (unsigned long)sc, (long)sc->Width, (long)sc->Height,
                    (long)depth, title);
        if (!send_frame(fd, T_DATA, line, n)) {
            UnlockIBase(ib);
            return FALSE;
        }
    }
    UnlockIBase(ib);

    if (bring)
        ScreenToFront(bring);
    else if ((flags & 1) && front)
        ScreenToBack(front);             /* what Amiga+M does */
    else if (want[0])
        return send_perr(fd, "no screen with that title");

    return send_frame(fd, T_END, NULL, 0);
}

/*
 * SUM: "has this part of the screen changed?" without sending it. The
 * region (front screen, its own pixels; 0x0 means all of it) is read
 * the way GRAB reads it - ReadPixelArray for a deep screen, pens and the
 * palette for a planar one - and folded into a 32-bit FNV-1a sum. Four
 * bytes cross the wire instead of the picture, which is what a slow
 * Amiga or a slow link wants from a "has it finished drawing?" poll.
 * Answer: DATA = u32 sum, u32 pixels summed; END.
 */
#define FNV_PRIME 16777619UL

static ULONG fnv(ULONG h, const UBYTE *p, LONG n)
{
    while (n-- > 0)
        h = (h ^ *p++) * FNV_PRIME;
    return h;
}

/*
 * A native screen's bitplanes, summed where they lie: no conversion to
 * pens, so a whole 640x256 screen takes a fraction of a second on a
 * 7 MHz 68000 instead of five. Only for bitplanes in chip RAM - an RTG
 * card's bitmap only looks like one. Bits just outside the region in
 * its first and last byte are masked off. A cheap rotate-and-xor, as a
 * 68000 has no 32-bit multiply.
 */
static BOOL plane_sum(struct BitMap *bm, LONG depth, LONG x, LONG y,
                      LONG w, LONG h, ULONG *sum)
{
    LONG p, row, b, bx0 = x >> 3, bx1 = (x + w - 1) >> 3;
    UBYTE m0 = (UBYTE)(0xff >> (x & 7));
    UBYTE m1 = (UBYTE)(0xff << (7 - ((x + w - 1) & 7)));
    ULONG hs = *sum;

    for (p = 0; p < depth; p++)
        if (!bm->Planes[p] || !(TypeOfMem(bm->Planes[p]) & MEMF_CHIP))
            return FALSE;
    for (p = 0; p < depth; p++) {
        for (row = y; row < y + h; row++) {
            const UBYTE *r = (const UBYTE *)bm->Planes[p]
                             + row * bm->BytesPerRow;
            for (b = bx0; b <= bx1; b++) {
                UBYTE v = r[b];
                if (b == bx0) v &= m0;
                if (b == bx1) v &= m1;
                hs = ((hs << 5) | (hs >> 27)) ^ v;
            }
        }
    }
    *sum = hs;
    return TRUE;
}

static BOOL cmd_sum(int fd, LONG x, LONG y, LONG w, LONG h)
{
    struct Screen *sc;
    ULONG sum = 2166136261UL, ib;
    LONG depth, band, row;
    UBYTE out[8];
    BOOL ok = TRUE;

    ib = LockIBase(0);
    sc = IntuitionBase->FirstScreen;
    UnlockIBase(ib);
    if (!sc)
        return send_perr(fd, "there are no screens open");
    if (w <= 0 || h <= 0) {
        x = y = 0;
        w = sc->Width;
        h = sc->Height;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sc->Width) w = sc->Width - x;
    if (y + h > sc->Height) h = sc->Height - y;
    if (w <= 0 || h <= 0 || w * 3 > MAX_PAYLOAD)
        return send_perr(fd, "that region is not on the screen");
    depth = bm_depth(sc->RastPort.BitMap);

    if (depth > 8) {
        LONG rowbytes = w * 3;
        UBYTE *buf;
        if (!g_cgfx)
            g_cgfx = OpenLibrary("cybergraphics.library", 40);
        if (!g_cgfx)
            return send_perr(fd, "no cybergraphics.library to read a deep "
                                 "screen with");
        band = MAX_PAYLOAD / rowbytes;
        if (band < 1) band = 1;
        if (!(buf = AllocMem(band * rowbytes, MEMF_ANY)))
            return send_perr(fd, "out of memory");
        for (row = 0; row < h; row += band) {
            LONG n = row + band > h ? h - row : band;
            __ReadPixelArray_base(g_cgfx, buf, 0, 0, (UWORD)rowbytes,
                                  &sc->RastPort, (UWORD)x, (UWORD)(y + row),
                                  (UWORD)w, (UWORD)n, RECTFMT_RGB);
            sum = fnv(sum, buf, n * rowbytes);
        }
        FreeMem(buf, band * rowbytes);
    } else {
        /* Pens, plus the palette they mean: a palette change is a
         * change on screen too. */
        static ULONG pal[256 * 3];
        struct RastPort temprp;
        LONG aligned = SCREEN_ALIGN(w), ncol = 1L << depth, r;
        UBYTE *pens;
        if (ncol > 256) ncol = 256;
        get_rgb32(sc->ViewPort.ColorMap, (ULONG)ncol, pal);
        sum = fnv(sum, (UBYTE *)pal, ncol * 12);
        if (plane_sum(sc->RastPort.BitMap, depth, x, y, w, h, &sum))
            goto done;
        band = MAX_PAYLOAD / aligned;
        if (band < 1) band = 1;
        temprp = sc->RastPort;
        temprp.Layer = NULL;
        temprp.BitMap = scratch_row(sc, aligned);
        if (!temprp.BitMap)
            return send_perr(fd, "out of memory for the scratch bitmap");
        if (!(pens = AllocMem(aligned * band, MEMF_ANY))) {
            scratch_free(temprp.BitMap);
            return send_perr(fd, "out of memory");
        }
        for (row = 0; row < h; row += band) {
            LONG n = row + band > h ? h - row : band;
            ReadPixelArray8(&sc->RastPort, (ULONG)x, (ULONG)(y + row),
                            (ULONG)(x + w - 1), (ULONG)(y + row + n - 1),
                            pens, &temprp);
            for (r = 0; r < n; r++)      /* only the region's columns */
                sum = fnv(sum, pens + r * aligned, w);
        }
        FreeMem(pens, aligned * band);
        scratch_free(temprp.BitMap);
    }
done:
    put_be32(out, sum);
    put_be32(out + 4, (ULONG)(w * h));
    ok = send_frame(fd, T_DATA, out, 8);
    return ok && send_frame(fd, T_END, NULL, 0);
}

static BOOL cmd_grab(int fd, const char *scrname)
{
    struct Screen *sc;
    BOOL haslock = FALSE, ok;
    UBYTE hdr[12];
    ULONG w, h;
    LONG depth;

    if (scrname[0]) {
        sc = LockPubScreen((STRPTR)scrname);
        if (!sc)
            return send_perr(fd, "no public screen by that name");
        haslock = TRUE;
    } else {
        /*
         * The frontmost screen, which is what "the screen" means to
         * somebody looking at the machine - and it is often a private
         * one (a game, an editor on its own screen), which no amount of
         * LockPubScreen would reach.
         *
         * LockIBase() makes reading the pointer safe. Nothing keeps the
         * screen from closing afterwards, so there is a race here: it
         * needs the screen to be closed during the few milliseconds of
         * the read. Said out loud rather than papered over.
         */
        ULONG ib = LockIBase(0);
        sc = IntuitionBase->FirstScreen;
        UnlockIBase(ib);
        if (!sc)
            return send_perr(fd, "there are no screens open");
    }

    w = (ULONG)sc->Width;
    h = (ULONG)sc->Height;
    depth = bm_depth(sc->RastPort.BitMap);
    if (!w || !h || (LONG)w * 3 > MAX_PAYLOAD) {
        if (haslock) UnlockPubScreen(NULL, sc);
        return send_perr(fd, "that screen is too wide to send a row at a time");
    }

    put_be32(hdr, w);
    put_be32(hdr + 4, h);
    put_be32(hdr + 8, 3);                /* bytes per pixel, RGB */
    if (!send_frame(fd, T_DATA, hdr, 12)) {
        if (haslock) UnlockPubScreen(NULL, sc);
        return FALSE;
    }

    ok = (depth > 8) ? screen_send_deep(fd, sc, w, h)
                     : screen_send_planar(fd, sc, w, h, depth);

    if (haslock) UnlockPubScreen(NULL, sc);
    return ok ? send_frame(fd, T_END, NULL, 0) : FALSE;
}

/* Join a target drawer and a name the way AmigaDOS wants it. */
static void speed_path(char *out, const char *target, LONG outsz)
{
    LONG n = (LONG)strlen(target);
    if (n > outsz - 20) n = outsz - 20;
    memcpy(out, target, n);
    if (n && out[n - 1] != ':' && out[n - 1] != '/')
        out[n++] = '/';
    strcpy(out + n, "wasabi-speed.tmp");
}

/*
 * Will `size` bytes fit on the volume `target` lives on, with room to
 * spare? Asked before a byte is accepted, because the obvious mistake -
 * a 256 MB test against RAM: on a machine with 66 MB - would otherwise
 * fill memory until something important fails to allocate.
 *
 * The margin is deliberate: a filesystem that is completely full is a
 * different kind of broken from one that is merely busy, and the test
 * is not worth leaving a machine in that state.
 */
#define SPEED_MARGIN_MB 8

static BOOL speed_room(const char *target, ULONG size, char *why, LONG whysz)
{
    struct InfoData id;
    BPTR lock;
    ULONG total, freemb, need = (size >> 20) + 1;

    lock = Lock((STRPTR)target, ACCESS_READ);
    if (!lock) {
        copystr(why, whysz, "no such drawer or volume");
        return FALSE;
    }
    if (!Info(lock, &id)) {
        UnLock(lock);
        copystr(why, whysz, "cannot read the volume's free space");
        return FALSE;
    }
    UnLock(lock);
    vol_megabytes(&id, &total, &freemb);
    if (id.id_DiskState == ID_WRITE_PROTECTED) {
        copystr(why, whysz, "that volume is write-protected");
        return FALSE;
    }
    if (freemb < need + SPEED_MARGIN_MB) {
        sprintf(why, "needs %lu MB plus %d MB spare, and only %lu MB is free",
                (unsigned long)need, SPEED_MARGIN_MB, (unsigned long)freemb);
        return FALSE;
    }
    return TRUE;
}

/* Swallow the upload a refused sink is already receiving, or its DATA
 * frames get read back as commands. Same rule as cmd_put's guard. */
static BOOL speed_drain(int fd, UBYTE *buf)
{
    for (;;) {
        UBYTE tag;
        LONG n;
        if (!recv_frame(fd, &tag, buf, &n))
            return FALSE;
        if (tag == T_END || tag != T_DATA)
            return TRUE;
    }
}

static BOOL cmd_speed(int fd, ULONG flags, ULONG size, const char *target)
{
    UBYTE *buf;
    BPTR fh = 0;
    char path[300];
    LONG i;
    BOOL tofile = target[0] != '\0';

    if (!size || size > (256UL << 20))
        return send_perr(fd, "size must be 1 byte to 256 MB");
    buf = AllocMem(MAX_PAYLOAD, MEMF_ANY);
    if (!buf)
        return send_perr(fd, "out of memory");

    if (tofile) {
        char why[100], msg[200];
        if (!(flags & 1) && !speed_room(target, size, why, sizeof(why))) {
            BOOL alive = speed_drain(fd, buf);
            FreeMem(buf, MAX_PAYLOAD);
            if (!alive)
                return FALSE;
            sprintf(msg, "cannot speedtest to %.40s: %.140s", target, why);
            return send_perr(fd, msg);
        }
        speed_path(path, target, sizeof(path));
        fh = Open(path, (flags & 1) ? MODE_OLDFILE : MODE_NEWFILE);
        if (!fh) {
            if (!(flags & 1) && !speed_drain(fd, buf)) {
                FreeMem(buf, MAX_PAYLOAD);
                return FALSE;
            }
            FreeMem(buf, MAX_PAYLOAD);
            return send_err(fd, (flags & 1)
                ? "no test file to read back - run the upload half first"
                : "cannot create the test file there");
        }
    }

    if (flags & 1) {                     /* source: Amiga -> client */
        ULONG left = size;
        for (i = 0; i < MAX_PAYLOAD; i++)
            buf[i] = (UBYTE)i;
        while (left) {
            LONG chunk = left > MAX_PAYLOAD ? MAX_PAYLOAD : (LONG)left;
            if (tofile) {
                chunk = Read(fh, buf, chunk);
                if (chunk <= 0)
                    break;              /* short file; END closes it honestly */
            }
            if (!send_frame(fd, T_DATA, buf, chunk)) {
                /* Delete it here too, not just on the way out below: a
                 * client that hangs up mid-read-back used to leave the
                 * whole test file on the volume - 64 MB of Dump:,
                 * measured - and the operator running a speedtest is by
                 * definition someone watching that volume's space. */
                if (fh) { Close(fh); DeleteFile(path); }
                FreeMem(buf, MAX_PAYLOAD);
                return FALSE;
            }
            left -= chunk;
        }
        if (tofile) {                    /* the read half also tidies up */
            Close(fh);
            DeleteFile(path);
        }
        FreeMem(buf, MAX_PAYLOAD);
        return send_frame(fd, T_END, NULL, 0);
    } else {                             /* sink: client -> Amiga */
        ULONG got = 0;
        BOOL wrote = TRUE;
        for (;;) {
            UBYTE tag;
            LONG n;
            if (!recv_frame(fd, &tag, buf, &n)) {
                if (fh) { Close(fh); DeleteFile(path); }
                FreeMem(buf, MAX_PAYLOAD);
                return FALSE;
            }
            if (tag == T_END)
                break;
            if (tag != T_DATA) {
                if (fh) { Close(fh); DeleteFile(path); }
                FreeMem(buf, MAX_PAYLOAD);
                return FALSE;
            }
            if (fh && wrote && Write(fh, buf, n) != n)
                wrote = FALSE;           /* keep draining, report after */
            got += n;
        }
        if (fh) Close(fh);
        FreeMem(buf, MAX_PAYLOAD);
        if (!wrote) {
            DeleteFile(path);
            return send_err(fd, "write failed part way - is the volume full?");
        }
        if (got != size) {
            if (fh) DeleteFile(path);
            return send_err(fd, "size mismatch");
        }
        return send_frame(fd, T_OK, NULL, 0);
    }
}

/*
 * Ask every mounted volume's handler to write out its dirty buffers.
 * Same locking shape as info_volumes(): collect under the DOS list lock,
 * talk to the handlers after releasing it - DoPkt to a handler that then
 * wants the list itself must not find us holding it.
 */
static void flush_volumes(void)
{
    struct MsgPort *ports[16];
    LONG count = 0, i;
    struct DosList *dl;

    dl = LockDosList(LDF_VOLUMES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_VOLUMES | LDF_READ)) && count < 16)
        if (dl->dol_Task)
            ports[count++] = dl->dol_Task;
    UnLockDosList(LDF_VOLUMES | LDF_READ);

    for (i = 0; i < count; i++)
        DoPkt(ports[i], ACTION_FLUSH, 0, 0, 0, 0, 0);
}

static BOOL cmd_reboot(int fd, ULONG flags)
{
    LONG i;
    (void)flags;                         /* bit 0 (cold) is accepted and
                                          * ignored: ColdReboot() is the only
                                          * reset exec sanctions a program to
                                          * make, so every reboot is cold */
    if (!send_frame(fd, T_OK, NULL, 0))
        return FALSE;
    say_goodbye("rebooting");
    /*
     * Close every client socket properly, not just the requester's: a
     * reset machine sends no FIN, so any connection left open here - a
     * debug stream in another terminal, say - would sit in recv()
     * staring at a peer that no longer exists.
     */
    for (i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].fd >= 0)
            CloseSocket(g_clients[i].fd);
    flush_volumes();
    /* Give the closes a moment to leave the wire, then go. */
    Delay(25);
    ColdReboot();
    return TRUE;                         /* not reached */
}

/*
 * Swap a verified sidecar in for the binary we run from.
 *
 * Renames only, no copying: the bytes that passed verification are
 * exactly the bytes installed - re-uploading could deliver something
 * else - and the previous binary stays one rename away as .bak, on the
 * Amiga, where a human at the keyboard can reach it when the network is
 * what broke. LoadSeg copied us into memory at launch and holds no lock
 * on the file, which is the same fact that makes self-update possible.
 *
 * If the second rename fails the first is undone, so a failed install
 * leaves a working daemon rather than a machine with no binary at all.
 */
static BOOL cmd_install(int fd, const char *sidecar)
{
    char self[128], bak[160];
    BPTR l;

    if (!GetProgramName(self, sizeof(self)) || !self[0])
        return send_perr(fd, "cannot tell which path I was started from");

    l = Lock((STRPTR)sidecar, ACCESS_READ);
    if (!l)
        return send_err(fd, "there is no such file to install");
    UnLock(l);

    sprintf(bak, "%.150s.bak", self);
    DeleteFile(bak);                     /* Rename will not clobber */
    if (!Rename(self, bak))
        return send_err(fd, "cannot move the current binary aside");
    if (!Rename((STRPTR)sidecar, self)) {
        Rename(bak, self);               /* undo; stay as we were */
        return send_err(fd, "cannot move the new binary into place");
    }
    return send_frame(fd, T_OK, NULL, 0);
}

/*
 * Stop, and stay stopped - g_restart is left clear so the exit path does
 * not relaunch us. This is how the throwaway instance that `wasabi
 * update` starts on a spare port is shut down once it has proved itself.
 */
/*
 * Exiting while a command runs is not a policy question but a memory
 * one: runner_entry is OUR code, and the shell unloads this segment the
 * moment the daemon exits - the Guru arrives minutes later, somewhere
 * that looks nothing like the cause. So quit and restart refuse while a
 * command holds a slot, unless forced. Reboot is exempt: the machine is
 * about to die anyway, and the runner with it.
 *
 * Forced, the commands get Ctrl-C and ten seconds; any that still run
 * are abandoned, and the exit path keeps this segment loaded for them
 * (leave_runners_behind) instead of refusing - a stuck command must
 * never again be something only a reboot can clear.
 */
static BOOL run_blocks_exit(int fd)
{
    if (!runs_busy())
        return FALSE;
    send_perr(fd, "a command is still running - wait for it to finish, "
                  "stop it with 'wasabi free N' ('wasabi slots' lists "
                  "them), or pass --force");
    return TRUE;
}

/* Everything before the last ':' or '/' is a path; the command name is
 * what remains. "C:Wait" and "Wait" are the same command. */
static const char *base_of(const char *s)
{
    const char *b = s, *p;
    for (p = s; *p; p++)
        if (*p == ':' || *p == '/')
            b = p + 1;
    return b;
}

/*
 * Ctrl-C a job's command: its own Shell when it can be found (exact,
 * see job_shell), else by the command name its CLI is executing,
 * matched against the first word of the RUN we started - the way an
 * operator at the keyboard would. The fallback is best effort: another
 * CLI running the same command also hears the Ctrl-C, which is what
 * Break-by-name has always risked on this machine.
 */
static void force_stop_run(struct RunJob *job)
{
    char word[64];
    LONG n, i, w = 0;

    if (job_break(job))
        return;
    if (job->cmd[0] == '"') {            /* a quoted command path */
        i = 1;
        while (job->cmd[i] && job->cmd[i] != '"' && w < 63)
            word[w++] = job->cmd[i++];
    } else {
        while (job->cmd[w] && job->cmd[w] != ' ' && w < 63) {
            word[w] = job->cmd[w];
            w++;
        }
    }
    word[w] = '\0';
    if (!word[0])
        return;

    n = ps_collect();
    for (i = 0; i < n; i++) {
        struct PsEnt *e = &g_ps[i];
        struct Task *t;
        BOOL alive = FALSE;
        if (!e->cmd[0] || !str_ieq(base_of(e->cmd), base_of(word)))
            continue;
        /* Re-find under Disable, same discipline as cmd_kill: the
         * snapshot pointer must be proven live before it is signalled. */
        Disable();
        for (t = (struct Task *)SysBase->TaskReady.lh_Head;
             t->tc_Node.ln_Succ; t = (struct Task *)t->tc_Node.ln_Succ)
            if (t == (struct Task *)e->addr) alive = TRUE;
        for (t = (struct Task *)SysBase->TaskWait.lh_Head;
             t->tc_Node.ln_Succ; t = (struct Task *)t->tc_Node.ln_Succ)
            if (t == (struct Task *)e->addr) alive = TRUE;
        if (alive)
            Signal((struct Task *)e->addr, SIGBREAKF_CTRL_C);
        Enable();
    }
}

/* Wait up to ticks/50 s for a job to finish; TRUE if it did. */
static BOOL job_wait(struct RunJob *job, LONG ticks)
{
    LONG i;
    for (i = 0; i < ticks && !job->done; i += 5)
        Delay(5);
    return job->done;
}

/* Ctrl-C every running command, give them ten seconds together, and
 * abandon whatever is still going. Afterwards no command holds a slot. */
static void force_run_down(void)
{
    LONG i, t;
    BOOL waiting;
    for (i = 0; i < MAX_JOBS; i++)
        if (g_jobs[i].active && !g_jobs[i].abandoned)
            force_stop_run(&g_jobs[i]);
    for (t = 0; t < 500; t += 5) {       /* ten seconds of grace */
        waiting = FALSE;
        for (i = 0; i < MAX_JOBS; i++)
            if (g_jobs[i].active && !g_jobs[i].abandoned &&
                !g_jobs[i].done)
                waiting = TRUE;
        if (!waiting)
            break;
        Delay(5);
    }
    for (i = 0; i < MAX_JOBS; i++) {
        struct RunJob *job = &g_jobs[i];
        if (!job->active || job->abandoned)
            continue;
        if (job->done)
            pump_run(job);               /* flush + EXIT to its client */
        else
            job_abandon(job, "the command ignored Ctrl-C; wasabid is "
                             "exiting and left it running");
    }
}

static BOOL exit_refused(int fd, ULONG flags)
{
    if (flags & 1)
        force_run_down();
    return run_blocks_exit(fd);
}

static BOOL cmd_quit(int fd, ULONG flags)
{
    if (exit_refused(fd, flags))
        return TRUE;
    if (!send_frame(fd, T_OK, NULL, 0))
        return FALSE;
    g_quit = TRUE;
    return TRUE;
}

/*
 * Relaunch ourselves - the fast half of self-update: `put C:wasabid`
 * then `restart` reloads the new binary without a full reboot. We only
 * flag it here; the actual relaunch happens in the exit path, AFTER the
 * listen socket is closed, so the fresh daemon can bind the same port
 * without racing us for it.
 */
static BOOL cmd_restart(int fd, ULONG flags)
{
    if (exit_refused(fd, flags))
        return TRUE;
    if (!send_frame(fd, T_OK, NULL, 0))
        return FALSE;
    g_restart = TRUE;
    g_quit = TRUE;
    return TRUE;
}

/* --- discovery ------------------------------------------------------ */

static int open_discovery(int port)
{
    int s, on = 1;
    struct sockaddr_in sa;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (void *)&on, sizeof(on));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        CloseSocket(s);
        return -1;
    }
    return s;
}

static void answer_probe(int s, int port)
{
    char buf[64], reply[160];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);        /* the inline wants socklen_t * */
    LONG n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                      (struct sockaddr *)&from, &fromlen);
    if (n <= 0)
        return;
    buf[n] = '\0';
    if (strncmp(buf, "WASABI?1", 8) != 0)
        return;
    /* Do not announce ourselves to anything we would refuse anyway. */
    if (!addr_allowed(ntohl(from.sin_addr.s_addr))) {
        note_refusal(ntohl(from.sin_addr.s_addr));
        return;
    }
    /* A daemon whose TLS could not start answers no client; say why
     * here, where `wasabi discover` and `ping` can show it. */
    n = sprintf(reply, "WASABI!1 %s %d %s%s%.80s\n", g_name, port,
                VERSION_STR, tls_ready() ? "" : " - unencrypted: ",
                tls_ready() ? "" : g_tls_why);
    sendto(s, reply, n, 0, (struct sockaddr *)&from, fromlen);
}

/* --- dispatch ------------------------------------------------------- */

static BOOL serve(int cl, UBYTE tag, UBYTE *p, LONG len)
{
    int fd = g_clients[cl].fd;
    char path[300];

    if (!g_clients[cl].hello) {
        char key[128];
        if (tag != T_HELLO)
            return send_perr(fd, "expected HELLO"), FALSE;
        if (len < 2 || get_be16(p) != PROTO_VERSION)
            return send_perr(fd, "protocol version mismatch"), FALSE;
        if (!get_str(p, len, 2, key, sizeof(key)))
            return send_perr(fd, "malformed HELLO"), FALSE;
        if (strcmp(key, g_key) != 0)
            return send_perr(fd, "bad key"), FALSE;
        g_clients[cl].hello = TRUE;
        {
            UBYTE w[320];
            LONG bl = (LONG)strlen(VERSION_STR);
            LONG kl = (LONG)strlen(CAPS_STR);
            LONG n = 0;
            w[n++] = 0; w[n++] = PROTO_VERSION;
            w[n++] = (UBYTE)(bl >> 8); w[n++] = (UBYTE)bl;
            memcpy(w + n, VERSION_STR, bl); n += bl;
            w[n++] = (UBYTE)(kl >> 8); w[n++] = (UBYTE)kl;
            memcpy(w + n, CAPS_STR, kl); n += kl;
            /* Appended after caps, same compatibility argument: an older
             * client stops at the banner and never sees it. */
            put_be32(w + n, g_refused_total); n += 4;
            return send_frame(fd, T_WELCOME, w, n);
        }
    }

    if (g_clients[cl].xfer == XF_PUT || g_clients[cl].xfer == XF_SINK)
        return put_frame(cl, tag, p, len);

    switch (tag) {
    case T_PING:
        return send_frame(fd, T_PONG, NULL, 0);

    case T_INFO:
        return cmd_info(fd);

    case T_LS:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_perr(fd, "bad path");
        return cmd_ls(fd, path);

    case T_GET:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_perr(fd, "bad path");
        return cmd_get(cl, fd, path);

    case T_PUT:
        if (len < 8 || !get_str(p, len, 8, path, sizeof(path)))
            return send_perr(fd, "bad PUT header");
        return cmd_put(cl, fd, get_be32(p), get_be32(p + 4), path);

    case T_DEL:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_perr(fd, "bad path");
        return DeleteFile(path) ? send_frame(fd, T_OK, NULL, 0)
                                : send_err(fd, "delete failed");

    case T_MKDIR:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_perr(fd, "bad path");
        {
            BPTR l = CreateDir(path);
            if (!l)
                return send_err(fd, "mkdir failed");
            UnLock(l);
            return send_frame(fd, T_OK, NULL, 0);
        }

    case T_RUN: {
        char cmd[512];
        if (len < 4 || !get_str(p, len, 4, cmd, sizeof(cmd)))
            return send_perr(fd, "bad RUN header");
        if (get_be32(p) & 2)             /* detach: start it and answer */
            return cmd_run_detached(fd, cmd);
        {
            const char *why;
            struct RunJob *job = start_run(cl, cmd, &why);
            UBYTE id[4];
            if (!job)
                return send_perr(fd, why);
            /* RUN_SLOT: the client asked to hear which slot, so that
             * its --max-time can stop exactly this command. Older
             * clients never set it and see the stream they always did. */
            if (get_be32(p) & 4) {
                put_be32(id, (ULONG)(job - g_jobs) + 1);
                if (!send_frame(fd, T_SLOT, id, 4))
                    return FALSE;
            }
        }
        return TRUE;                     /* output follows from pump_run */
    }

    case T_PS:
        return cmd_ps(fd, len >= 4 ? get_be32(p) : 0);

    case T_SLOTS:
        return cmd_slots(fd);

    case T_FREE:
        if (len < 4)
            return send_perr(fd, "bad FREE header");
        return cmd_free(fd, get_be32(p), len >= 8 ? get_be32(p + 4) : 0);

    case T_KILL: {
        char target[64];
        if (len < 4 || !get_str(p, len, 4, target, sizeof(target)))
            return send_perr(fd, "bad KILL header");
        return cmd_kill(fd, get_be32(p), target);
    }

    case T_SCREEN: {
        char want[100];
        if (len < 4 || !get_str(p, len, 4, want, sizeof(want)))
            want[0] = '\0';
        return cmd_screenctl(fd, len >= 4 ? get_be32(p) : 0, want);
    }

    case T_GRAB: {
        char scr[64];
        if (len < 2 || !get_str(p, len, 0, scr, sizeof(scr)))
            scr[0] = '\0';
        return cmd_grab(fd, scr);
    }

    case T_INPUT: {
        if (len < 10)
            return send_perr(fd, "bad INPUT header");
        return cmd_input(fd,
                         (p[0] << 8) | p[1],
                         (p[2] << 8) | p[3],
                         (p[4] << 8) | p[5],
                         (WORD)((p[6] << 8) | p[7]),
                         (WORD)((p[8] << 8) | p[9]));
    }

    case T_KEY:
        if (len < 2)
            return send_perr(fd, "bad KEY header");
        return cmd_key(fd, (p[0] << 8) | p[1], p + 2, len - 2);

    case T_INPUTS:
        return cmd_inputs(fd, p, len);

    case T_WINDOWS:
        return cmd_windows(fd);

    case T_UI:
        return cmd_ui(fd, len >= 4 ? get_be32(p) : 0);

    case T_UIACT:
        if (len < 12)
            return send_perr(fd, "bad UIACT header");
        return cmd_uiact(fd, get_be32(p), get_be32(p + 4), get_be32(p + 8));

    case T_AREXX:
        return cmd_arexx(cl, fd, p, len);

    case T_PORTS:
        return cmd_ports(fd);

    case T_SUM:
        if (len < 8)
            return send_perr(fd, "bad SUM header");
        return cmd_sum(fd, (WORD)get_be16(p), (WORD)get_be16(p + 2),
                       (WORD)get_be16(p + 4), (WORD)get_be16(p + 6));

    case T_CLIP:
        if (len < 4)
            return send_perr(fd, "bad CLIP header");
        return cmd_clip(fd, get_be32(p), p + 4, len - 4);

    case T_LIVE:
        return cmd_live(cl, fd, len >= 4 ? get_be32(p) : 0);

    case T_HEALTH: {
        /* Static: 2 KB is a quarter of the shell's 8 KB stack. */
        static char report[2048];
        LONG n = health_report(report, sizeof(report));
        if (!send_frame(fd, T_DATA, report, n))
            return FALSE;
        return send_frame(fd, T_END, NULL, 0);
    }

    case T_SPEED: {
        char target[200];
        if (len < 8)
            return send_perr(fd, "bad SPEED header");
        if (len == 8)
            target[0] = '\0';            /* older client: storage-free mode */
        else if (!get_str(p, len, 8, target, sizeof(target)))
            return send_perr(fd, "bad SPEED target");
        return cmd_speed(fd, get_be32(p), get_be32(p + 4), target);
    }

    case T_REBOOT:
        return cmd_reboot(fd, len >= 4 ? get_be32(p) : 0);

    case T_RESTART:
        return cmd_restart(fd, len >= 4 ? get_be32(p) : 0);

    case T_QUIT:
        return cmd_quit(fd, len >= 4 ? get_be32(p) : 0);

    case T_INSTALL:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_perr(fd, "bad path");
        return cmd_install(fd, path);

    case T_DEBUG:
        if (g_trial)
            return send_perr(fd, "this is a trial instance - it installs "
                                 "no patches, so it cannot stream");
        if (g_dbg_client >= 0)
            return send_perr(fd, "the debug stream is already in use");
        debug_start();
        g_dbg_client = cl;
        g_dbg_seq = 0;
        stream_greet(cl, 0, &g_dbg_seq);
        return TRUE;                     /* LOG frames follow from the pump */

    case T_SNOOP: {
        char pat[64];
        char why[64];
        if (len < 4 || !get_str(p, len, 4, pat, sizeof(pat)))
            return send_perr(fd, "bad SNOOP header");
        if (g_trial)
            return send_perr(fd, "this is a trial instance - it installs "
                                 "no patches, so it cannot snoop");
        if (g_snoop_client >= 0)
            return send_perr(fd, "the snoop stream is already in use");
        if (!snoop_start(pat, get_be32(p), why, sizeof(why))) {
            char msg[200];
            sprintf(msg, "snoop self-test failed (%.60s) - the patch "
                         "trampoline and this build disagree, so snoop "
                         "will not run", why);
            return send_perr(fd, msg);
        }
        g_snoop_client = cl;
        g_snoop_seq = 0;
        /* The same note the debug stream gets - but not twice to a
         * client whose combined attach already saw it there. */
        if (g_dbg_client != cl)
            stream_greet(cl, 1, &g_snoop_seq);
        return TRUE;
    }

    default:
        return send_perr(fd, "unknown command");
    }
}

/* --- main ----------------------------------------------------------- */

static void drop(int cl)
{
    LONG j;
    for (j = 0; j < MAX_JOBS; j++) {     /* a run outlives its client */
        struct RunJob *job = &g_jobs[j];
        if (!job->active || job->client != cl)
            continue;
        if (job->read) { Close(job->read); job->read = 0; }
        job->client = -1;                /* the job stays active: the
                                          * runner is still alive, and
                                          * pump_run() cleans up and frees
                                          * the slot when it finishes */
    }
    /* Free the subscription as well as the patch. These two used to be
     * one act, back when the patch layer owned the client index; now
     * that it owns only the ring, forgetting the second half leaves a
     * stale index pointing at a slot the next caller will be given -
     * and that caller gets LOG frames instead of a welcome. */
    if (g_dbg_client == cl) {
        debug_stop();
        g_dbg_client = -1;
    }
    if (g_snoop_client == cl) {
        snoop_stop();
        g_snoop_client = -1;
    }
    live_free(cl);
    posted_forget(cl);
    xfer_abort(cl);
    if (g_clients[cl].tls) {
        tls_bye(g_clients[cl].tls);
        tls_free(g_clients[cl].tls);
        g_clients[cl].tls = NULL;
    }
    g_clients[cl].secure = FALSE;
    CloseSocket(g_clients[cl].fd);
    g_clients[cl].fd = -1;
    g_clients[cl].hello = FALSE;
}

/*
 * Prove a freshly uploaded binary can actually run, before it is allowed
 * to replace a working one. Once the old daemon has exited to relaunch,
 * nothing is left running that could roll a bad build back - so the
 * check has to happen while the old daemon is still alive and in charge,
 * which means the new binary has to be able to check itself.
 *
 * What is worth checking is the dependency that actually fails in
 * practice: the TCP/IP stack. Open bsdsocket.library, prove a socket can
 * be created and bound, say so, exit 0. A binary that cannot do this
 * would come up dead and take the machine off the network with it.
 *
 * Deliberately NOT checked: anything that patches the system. A process
 * that SetFunction()s and then exits is exactly the "do not let this
 * binary unload" hazard the teardown rules exist to avoid.
 */
static int selftest(const char *nonce)
{
    int s;
    struct sockaddr_in sa;

    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) {
        Printf("%s selftest: FAILED - no bsdsocket.library\n",
               (LONG)VERSION_STR);
        return RETURN_FAIL;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        CloseLibrary(SocketBase);
        Printf("%s selftest: FAILED - cannot create a socket\n",
               (LONG)VERSION_STR);
        return RETURN_FAIL;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = 0;                     /* any free port; only the bind
                                          * matters, and this cannot clash
                                          * with the daemon still running */
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        CloseSocket(s);
        CloseLibrary(SocketBase);
        Printf("%s selftest: FAILED - cannot bind a socket\n",
               (LONG)VERSION_STR);
        return RETURN_FAIL;
    }
    CloseSocket(s);
    /* TLS is reported, not required here: an emulator reached over
     * loopback needs none. Over a network, `wasabi update` proves it
     * for real - its probe of the trial instance is a TLS connection. */
    {
        const char *why = tls_open();
        tls_close();
        if (why)
            Printf("%s selftest: note - TLS not ready (%s); only this "
                   "machine itself could connect\n", (LONG)VERSION_STR,
                   (LONG)why);
    }
    CloseLibrary(SocketBase);
    SocketBase = NULL;
    /*
     * The marker is the point. Exit status alone proves nothing - plenty
     * of ordinary commands exit 0 when handed an argument they do not
     * understand (C:Echo prints it and returns 0), and one of those
     * installed as the daemon is a machine off the network. Echoing back
     * the caller's nonce also proves this line came from THIS run and
     * not from a stale file or a lucky string.
     */
    Printf("wasabid-selftest-ok %s %s\n", (LONG)nonce, (LONG)VERSION_STR);
    return RETURN_OK;
}

static int daemon_main(int argc, char **argv)
{
    int listen_fd = -1, disco_fd = -1, port = DEF_PORT, i;
    int dbg_was = -1, snoop_was = -1;    /* stream clients at teardown */
    BOOL stuck;                          /* a patch outlived its removal */
    UBYTE *payload;

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return selftest(argc > 2 ? argv[2] : "-");

    /*
     * Arguments, in any order:
     *   <port>          listen somewhere else - lets a trial instance run
     *                   beside a live one without a bind clash
     *   name <id>       what discovery replies call this machine, so two
     *                   Amigas on one LAN are telling apart; overrides
     *                   ENV:HOSTNAME
     *   allow <cidr>    also answer this range, e.g. a Tailscale 100.64/10
     *   allow any       answer anybody at all; see the warning below
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "name") == 0 && i + 1 < argc) {
            char *p;
            strncpy(g_name, argv[++i], sizeof(g_name) - 1);
            g_name[sizeof(g_name) - 1] = '\0';
            for (p = g_name; *p; p++)
                if (*p == ' ') *p = '-'; /* the reply is space-delimited */
            g_name_set = TRUE;
        } else if (strcmp(argv[i], "trial") == 0) {
            g_trial = TRUE;
        } else if (strcmp(argv[i], "plain") == 0) {
            g_allow_plain = TRUE;
        } else if (strcmp(argv[i], "allow") == 0 && i + 1 < argc) {
            const char *what = argv[++i];
            if (strcmp(what, "any") == 0)
                g_allow_any = TRUE;
            else if (g_allow_n < MAX_ALLOW &&
                     parse_cidr(what, &g_allow[g_allow_n]))
                g_allow_n++;
            else
                Printf("wasabid: ignoring bad allow '%s'\n", (LONG)what);
        } else {
            LONG p = atol(argv[i]);
            if (p > 0 && p < 65536)
                port = (int)p;
        }
    }
    g_port = port;                       /* remembered for restart */
    /* Replayed on restart, so a self-update does not silently drop the
     * allow-list and lock the operator out of their own machine. */
    {
        LONG k, n = 0;
        for (k = 0; k < g_allow_n && n < (LONG)sizeof(g_extra_args) - 40; k++) {
            ULONG b = g_allow[k].base, m = g_allow[k].mask;
            LONG bits = 0;
            while (m & 0x80000000UL) { bits++; m <<= 1; }
            n += sprintf(g_extra_args + n, " allow %lu.%lu.%lu.%lu/%ld",
                         (unsigned long)((b >> 24) & 255),
                         (unsigned long)((b >> 16) & 255),
                         (unsigned long)((b >> 8) & 255),
                         (unsigned long)(b & 255), (long)bits);
        }
        if (g_name_set && n < (LONG)sizeof(g_extra_args) - 40)
            n += sprintf(g_extra_args + n, " name %s", g_name);
        /* Bounded like the two above it: with a full allow-list and a
         * long name, n reaches 124 here, and " allow any" needs 11 -
         * which lands in g_allow[0], silently rewriting the running
         * daemon's own allow-list. */
        if (g_allow_any && n < (LONG)sizeof(g_extra_args) - 11)
            n += sprintf(g_extra_args + n, " allow any");
        if (g_allow_plain && n < (LONG)sizeof(g_extra_args) - 7)
            n += sprintf(g_extra_args + n, " plain");
    }

    /* The daemon has no console either: a requester raised by ls, get or
     * put would hang the whole loop, not just one command. */
    {
        struct Task *me = FindTask(NULL);
        if (me->tc_Node.ln_Type == NT_PROCESS)
            ((struct Process *)me)->pr_WindowPtr = (APTR)-1;
        patches_set_daemon_task(me);
        /* Priority 1, and deliberately: when a crashing task Signal()s
         * us from the Alert patch, being the higher-priority ready task
         * is what puts the guru report on the wire before the original
         * Alert() freezes the display. The rest of the time the loop is
         * asleep in WaitSelect, so nobody pays for this. */
        SetTaskPri(me, 1);
    }

    /*
     * The previous life's guru, if any - two places to look.
     *
     * ExecBase->LastAlert first: on real hardware exec preserves it
     * across a warm reboot (that is how the boot-time guru screen
     * knows what to show). On Emu68 it comes back FFFFFFFF no matter
     * what died - proven by poking a value in and rebooting - so the
     * check costs nothing there and works where exec cooperates.
     * Retire whatever is found, or one old crash would be
     * rediscovered by every future daemon start.
     *
     * Then the black box, which is what actually survives on Emu68;
     * checked second so its richer report (task name included) wins
     * the note file when both exist.
     */
    if (!g_trial) {                      /* shared state: not ours to take */
        guru_claim();
        guru_boot_check();
    }

    /* Shared file, same rule as the shared vectors and the shared black
     * box: a trial instance is a guest on this machine and rewrites
     * nothing the live daemon owns. */
    if (!g_trial)
        refusals_load();

    for (i = 0; i < MAX_CLIENTS; i++)
        g_clients[i].fd = -1;

    if (GetVar("wasabi.key", g_key, sizeof(g_key), 0) <= 0)
        g_key[0] = '\0';

    /* Name for discovery replies: two Amigas answering as "amiga" are
     * indistinguishable. The 'name' argument wins; else Roadshow and
     * rondoval's stack both set ENV:HOSTNAME; without either the old
     * default stands. The name field is space-delimited, so spaces
     * become dashes. */
    if (!g_name_set) {
        if (GetVar("HOSTNAME", g_name, sizeof(g_name), 0) > 0) {
            char *p;
            for (p = g_name; *p; p++)
                if (*p == ' ') *p = '-';
        } else
            strcpy(g_name, "amiga");
    }

    /*
     * From here on a failure must give the black box back before it
     * returns. Leaving the region claimed by a process that then exits
     * means every later daemon on this boot gets AllocAbs = NULL and
     * the machine silently has no crash reporting at all - a good way
     * to lose the one guru that mattered, days later. The Alert patch
     * is not armed yet, so releasing here is unconditionally safe.
     */
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) {
        Printf("wasabid: no bsdsocket.library - is the TCP/IP stack up?\n");
        guru_release();
        return RETURN_FAIL;
    }
    payload = AllocMem(MAX_PAYLOAD, MEMF_ANY);
    if (!payload) {
        CloseLibrary(SocketBase);
        Printf("wasabid: out of memory\n");
        guru_release();
        return RETURN_FAIL;
    }

    {
        int on = 1;
        struct sockaddr_in sa;
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on));
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (listen_fd < 0 ||
            bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            listen(listen_fd, 4) < 0) {
            Printf("wasabid: cannot listen on port %ld\n", (long)port);
            goto out;
        }
    }
    disco_fd = open_discovery(port);

    /* Armed for the daemon's whole life, not per-subscription like the
     * stream patches: a guru waits for its first listener, and the
     * LastAlert path needs nothing at all. A trial instance patches
     * nothing: see g_trial. */
    if (!g_trial)
        guru_arm();

    /*
     * Encryption is optional (the user's decision, 14 October 2026: not
     * everyone has AmiSSL). With AmiSSL and keys, connections from the
     * network must be TLS - plain ones are let in only from 127.x or
     * with the 'plain' option. Without them - AmiSSL missing, its
     * assign not made yet at boot, the keys not copied over - the
     * daemon runs plain and tries TLS again every half minute; the
     * client still refuses to go plain to a host it has reached
     * encrypted before (its pins).
     */
    g_tls_why = tls_open();
    g_tls_tried = now_secs();
    if (g_tls_why)
        Printf("wasabid: no encryption (%s) - connections are plain; "
               "trying again every %ld s\n",
               (LONG)g_tls_why, (LONG)TLS_RETRY_SECS);
    else if (g_allow_plain)
        Printf("wasabid: 'plain' is set - unencrypted clients are let in "
               "too\n");

    rexxport_open();                     /* the WASABI ARexx port */

    Printf("%s listening on port %ld%s. Break C to stop.\n",
           (LONG)VERSION_STR, (long)port,
           (LONG)(g_key[0] ? "" : " (NO KEY SET - see ENV:wasabi.key)"));
    if (g_allow_any)
        Printf("wasabid: WARNING - 'allow any' is set. This daemon runs "
               "arbitrary\n         commands and will now answer ANY "
               "address, including the\n         open internet. Do not "
               "leave it like this.\n");

    while (!g_quit) {
        fd_set rd;
        struct timeval tv;
        ULONG sigs = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F | posted_sigmask() |
                     rexxport_sigmask();
        int nfds = listen_fd;
        LONG n;
        BOOL pend;

        FD_ZERO(&rd);
        FD_SET(listen_fd, &rd);
        if (disco_fd >= 0) {
            FD_SET(disco_fd, &rd);
            if (disco_fd > nfds) nfds = disco_fd;
        }
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd >= 0) {
                FD_SET(g_clients[i].fd, &rd);
                if (g_clients[i].fd > nfds) nfds = g_clients[i].fd;
            }
        }

        /* Poll briskly while a command or the debug stream is producing
         * output; idle otherwise. Sashimi writes to its temp file on each
         * newline, so a growing file has no readable-fd to select on -
         * only a short timer catches it. */
        {
            BOOL busy = (runners_alive() || g_dbg_client >= 0 ||
                         g_snoop_client >= 0);
            /* A waiting ARexx answer wakes us by signal; the timer is
             * only for its time limit, so a second is plenty. */
            if (!busy && posted_count()) {
                tv.tv_secs = 1;
                tv.tv_micro = 0;
            } else {
            tv.tv_secs  = busy ? 0 : 2;
            tv.tv_micro = busy ? 50000 : 0;
            }
        }

        /* Bytes TLS has already decrypted do not make the socket
         * readable again: look, and do not sleep on them. */
        pend = FALSE;
        for (i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i].fd >= 0 && g_clients[i].secure &&
                tls_pending(g_clients[i].tls) > 0)
                pend = TRUE;
        if (pend || gets_pending()) {
            tv.tv_secs = 0;
            tv.tv_micro = 0;
        }

        n = WaitSelect(nfds + 1, &rd, NULL, NULL, &tv, &sigs);
        if (n < 0)
            FD_ZERO(&rd);

        {
            ULONG tnow = now_secs();
            /* A handshake that stalls holds a client slot: bounded. */
            for (i = 0; i < MAX_CLIENTS; i++)
                if (g_clients[i].fd >= 0 && !g_clients[i].secure &&
                    tnow - g_clients[i].since > HANDSHAKE_SECS)
                    drop(i);
            if (!tls_ready() && tnow - g_tls_tried >= TLS_RETRY_SECS) {
                g_tls_why = tls_open();
                g_tls_tried = tnow;
            }
        }

        if (sigs & SIGBREAKF_CTRL_C)
            break;

        for (i = 0; i < MAX_JOBS; i++)
            if (g_jobs[i].active && !pump_run(&g_jobs[i]))
                drop(g_jobs[i].client);  /* only reachable with a client:
                                          * headless pump never says FALSE */

        /*
         * Drain what the patches captured. They hand over bytes and
         * whole lines; deciding they are LOG frames on a socket is this
         * side's business, which is the entire point of the split.
         */
        if (g_dbg_client >= 0 && !pump_debug_stream())
            drop(g_dbg_client);
        if (g_snoop_client >= 0 && !pump_snoop_stream())
            drop(g_snoop_client);

        pump_posted();
        rexxport_pump();
        pump_gets(payload);              /* a chunk of each download */

        {
            char line[200];
            if (guru_take_live(line, sizeof(line)))
                stream_note(line, (LONG)strlen(line));
            guru_retry_note();
        }

        /*
         * Heartbeat: an empty LOG on each subscribed stream every few
         * seconds. The client renders nothing - there is nothing to
         * render - but its silence timer resets, so a machine that
         * Gurus or reboots behind a stream's back is noticed in
         * seconds instead of holding the terminal open forever.
         *
         * A heartbeat is a courtesy, not a delivery. If the socket
         * cannot take it RIGHT NOW - the client's machine is asleep,
         * the Wi-Fi link is blinking, the buffer is full of unread
         * stream - skip it rather than sit in io_wait for ten seconds
         * and then drop a subscriber whose only crime was a flaky hop.
         * A peer that is truly gone still gets reclaimed: the
         * unacknowledged bytes already in flight make the stack time
         * the connection out, and the next send into it fails.
         */
        if (g_dbg_client >= 0 || g_snoop_client >= 0) {
            ULONG hbnow = now_secs();
            if (hbnow - g_hb_last >= HB_SECS) {
                g_hb_last = hbnow;
                if (g_dbg_client >= 0 &&
                    !hb_send(g_clients[g_dbg_client].fd, 0, &g_dbg_seq))
                    drop(g_dbg_client);
                if (g_snoop_client >= 0 &&
                    !hb_send(g_clients[g_snoop_client].fd, 1,
                             &g_snoop_seq))
                    drop(g_snoop_client);
            }
        }

        if (!g_trial)
            refusals_save(FALSE);        /* rate-limited to once a minute */

        if (n <= 0 && !pend)
            continue;

        if (disco_fd >= 0 && FD_ISSET(disco_fd, &rd))
            answer_probe(disco_fd, port);

        if (FD_ISSET(listen_fd, &rd)) {
            struct sockaddr_in peer;
            socklen_t peerlen = sizeof(peer);
            int fd = accept(listen_fd, (struct sockaddr *)&peer, &peerlen);
            if (fd >= 0) {
                ULONG a = ntohl(peer.sin_addr.s_addr);
                if (!addr_allowed(a)) {
                    note_refusal(a);
                    CloseSocket(fd); /* before HELLO: it never gets a turn */
                    fd = -1;
                } else
                    note_accept(a);
            }
            if (fd >= 0) {
                int slot = -1;
                for (i = 0; i < MAX_CLIENTS; i++)
                    if (g_clients[i].fd < 0) { slot = i; break; }
                if (slot < 0) {
                    CloseSocket(fd);
                } else {
                    int on = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                               (void *)&on, sizeof(on));
                    g_clients[slot].fd = fd;
                    g_clients[slot].hello = FALSE;
                    g_clients[slot].secure = FALSE;
                    g_clients[slot].since = now_secs();
                    /*
                     * TLS or plain is decided by the client's first byte,
                     * in the client loop: 127.x (this machine, which is
                     * also how the PC reaches an FS-UAE emulator) may
                     * always skip TLS; others only when TLS cannot be had
                     * here or 'plain' was given.
                     */
                    g_clients[slot].local =
                        (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
                    g_clients[slot].tls = NULL;  /* the first byte says */
                }
            }
        }

        for (i = 0; i < MAX_CLIENTS; i++) {
            UBYTE tag;
            LONG len;
            if (g_clients[i].fd < 0)
                continue;
            if (!g_clients[i].secure) {
                int r;
                if (!FD_ISSET(g_clients[i].fd, &rd))
                    continue;
                if (!g_clients[i].tls) {
                    /*
                     * A TLS ClientHello starts 0x16; a HELLO frame 0x01.
                     * Plain is let in from this machine itself, when
                     * TLS cannot be had here (no AmiSSL, no keys - the
                     * startup line and discovery say so), or when the
                     * daemon was started with 'plain'. Otherwise the
                     * client is told why, in a frame it can read.
                     */
                    UBYTE first = 0;
                    if (recv(g_clients[i].fd, &first, 1, MSG_PEEK) != 1) {
                        drop(i);
                        continue;
                    }
                    if (first != 0x16) {
                        if (g_clients[i].local || !tls_ready() ||
                            g_allow_plain) {
                            g_clients[i].secure = TRUE;  /* plain */
                            continue;
                        }
                        send_perr(g_clients[i].fd, "this Amiga accepts only "
                                  "encrypted connections - the client needs "
                                  "its keys (wasabi keys), or start wasabid "
                                  "with 'plain'");
                        drop(i);
                        continue;
                    }
                    g_clients[i].tls = tls_start(g_clients[i].fd);
                    if (!g_clients[i].tls) {
                        drop(i);         /* TLS asked for, none here */
                        continue;
                    }
                }
                r = tls_step(g_clients[i].tls);
                if (r == 2) {            /* wants to write: rare, brief */
                    fd_set w;
                    struct timeval wt;
                    FD_ZERO(&w);
                    FD_SET(g_clients[i].fd, &w);
                    wt.tv_secs = 1;
                    wt.tv_micro = 0;
                    WaitSelect(g_clients[i].fd + 1, NULL, &w, NULL, &wt,
                               NULL);
                    r = tls_step(g_clients[i].tls);
                }
                if (r == 1)
                    g_clients[i].secure = TRUE;
                else if (r < 0)
                    drop(i);             /* wrong certificate, or not TLS */
                continue;
            }
            if (!FD_ISSET(g_clients[i].fd, &rd) &&
                tls_pending(g_clients[i].tls) <= 0)
                continue;
            if (!recv_frame(g_clients[i].fd, &tag, payload, &len)) {
                drop(i);
                continue;
            }
            if (!serve(i, tag, payload, len))
                drop(i);
        }
    }

    Printf("wasabid: stopping\n");

out:
    /*
     * Break at the console cannot be refused the way quit and restart
     * are, and the runner executes this segment's code - exiting now
     * would unload it under a live process. So wait, and say why:
     * nothing but the command finishing can hurry this.
     */
    for (i = 0; i < MAX_JOBS; i++) {
        struct RunJob *job = &g_jobs[i];
        if (job->active && !job->abandoned && !job->done) {
            Printf("wasabid: a command is still running - waiting for it "
                   "before exiting ('wasabi free' or a forced quit lets "
                   "go of it)\n");
            while (!job->done && !job->abandoned)
                Delay(10);
        }
        if (job->active && job->done) {  /* the cleanup pump_run would do */
            if (job->read) { Close(job->read); job->read = 0; }
            DeleteFile(job->outname);
            job->active = FALSE;
        }
    }
    leave_runners_behind();
    if (!g_trial)
        refusals_save(TRUE);
    say_goodbye(g_restart ? "restarting" : "stopping");
    dbg_was = g_dbg_client;              /* the stops below clear these */
    snoop_was = g_snoop_client;
    debug_stop();                        /* removes the patch */
    g_dbg_client = -1;
    snoop_stop();
    g_snoop_client = -1;
    guru_disarm();
    /* After the patch is gone, so nothing can be writing to it. The
     * next instance claims the same address and reads what is still
     * there - a reboot does not clear RAM, and neither does a free. */
    guru_release();
    /*
     * A task may still be between the snoop stub's use-count bump and
     * its rts. Wait for it: the patched set includes SystemTagList and
     * Execute, which run whole commands, so "a moment" was optimistic
     * at 2 seconds. Five is enough for anything short, and a straggler
     * that outlasts it is not abandoned - patches_stuck() counts
     * snoop_busy(), so the process parks below rather than unloading
     * the segment somebody is still standing in.
     */
    {
        int w;
        for (w = 0; snoop_busy() && w < 125; w++)
            Delay(2);
    }
    patches_closelibs();                 /* now nobody is inside them */
    /*
     * A patch we could not remove is a live pointer into this segment,
     * and the shell frees the segment the moment main() returns. The
     * next call through that vector then jumps into memory that has
     * been handed to somebody else - which surfaces later, somewhere
     * else, as a corrupt memory list or a wild address. This used to
     * print a warning and return anyway; the warning went to a
     * detached daemon's NIL: output, where nobody has ever read one.
     *
     * So: say it where it will be seen, and then do not unload. One
     * stranded process costs 50 KB. Guruing the machine minutes later
     * costs the operator their afternoon and their trust in the log.
     */
    stuck = patches_stuck();
    if (!stuck)
        patches_free();
    if (g_rowbuf)
        FreeMem(g_rowbuf, LIVE_RECT_MAX);
    if (g_rx_items)
        FreeMem(g_rx_items, sizeof(struct UiItem) * RX_MAX_ITEMS);
    if (stuck) {
        char w[200];
        LONG n = sprintf(w, "[wasabi: WARNING - a SetFunction patch could "
                            "not be removed (someone patched over it). "
                            "This daemon is staying resident rather than "
                            "unloading into a guru; reboot when "
                            "convenient.]\n");
        /* The stops above cleared the stream indices; put them back for
         * exactly this one message, which is the last thing worth
         * saying on a stream. */
        g_dbg_client = dbg_was;
        g_snoop_client = snoop_was;
        stream_note(w, n);
        g_dbg_client = g_snoop_client = -1;
        Printf("wasabid: WARNING - a SetFunction patch could not be removed "
               "(someone patched over it). Staying resident: this binary "
               "must not unload. Reboot when convenient.\n");
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        xfer_abort(i);
        live_free(i);                   /* ~7 MB per live viewer at 1280x960:
                                         * AllocMem is not given back when a
                                         * program ends (audit 4) */
        if (g_clients[i].tls) {
            tls_bye(g_clients[i].tls);
            tls_free(g_clients[i].tls);
            g_clients[i].tls = NULL;
        }
        if (g_clients[i].fd >= 0)
            CloseSocket(g_clients[i].fd);
    }
    tls_close();
    if (disco_fd >= 0) CloseSocket(disco_fd);
    if (listen_fd >= 0) CloseSocket(listen_fd);
    FreeMem(payload, MAX_PAYLOAD);
    if (g_cgfx) CloseLibrary(g_cgfx);
    if (g_keymap) CloseLibrary(g_keymap);
    if (g_gadtools) CloseLibrary(g_gadtools);
    mui3_close();
    rexxport_close();
    posted_shutdown();
    /* A rexx message left with a slow host still needs the library to
     * be freed one day; keep it open in that case. */
    if (g_rexxsys && !g_post_port) CloseLibrary(g_rexxsys);
    CloseLibrary(SocketBase);

    /*
     * Relaunch now that the port is free. GetProgramName() gives the path
     * we were invoked by (C:wasabid, RAM:wasabid.b6, ...), so the fresh
     * daemon keeps our identity and port. Run detaches it; we then exit.
     */
    if (g_restart) {
        /* 13 fixed + a 127-char program path + a 5-digit port + 127
         * chars of replayed arguments + NUL = 273 worst case. */
        char self[128], cmd[288];
        if (GetProgramName(self, sizeof(self)) && self[0]) {
            /* Quoted: a daemon started from a path with a space in it
             * must come back from restart too. */
            sprintf(cmd, "Run >NIL: \"%s\" %d%s", self, g_port, g_extra_args);
            Execute(cmd, 0, 0);
        }
    }

    /* The replacement daemon has the port; this process now exists only
     * to keep its code in memory, because something still points into
     * it. Returning here is what would guru the machine later. */
    if (stuck)
        for (;;)
            Delay(250);

    return RETURN_OK;
}

/*
 * The daemon's code is budgeted for an 8 KB stack, but it gets whatever
 * the Shell that ran it had: 4 KB from OS 3.1's startup-sequence. On
 * that, listing a GadTools window (Input prefs, 3.1) went over the edge
 * and took the machine with it. So when the stack we were given is
 * smaller than STACK_MIN, run on one of our own. Everything that lives
 * across StackSwap() is static: locals on the old stack are not
 * reachable from the new one.
 */
#define STACK_MIN 16384
static struct StackSwapStruct g_sss;
static int g_argc, g_rc;
static char **g_argv;

/* No arguments and no result, so the compiler leaves nothing of its own
 * on the new stack for after the swap back. */
static void __attribute__((noinline)) daemon_on_own_stack(void)
{
    g_rc = daemon_main(g_argc, g_argv);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);

    if ((ULONG)((UBYTE *)me->tc_SPUpper - (UBYTE *)me->tc_SPLower) >= STACK_MIN)
        return daemon_main(argc, argv);
    g_sss.stk_Lower = AllocMem(STACK_MIN, MEMF_ANY);
    if (!g_sss.stk_Lower)
        return daemon_main(argc, argv);  /* as before, on the small one */
    g_sss.stk_Upper = (ULONG)g_sss.stk_Lower + STACK_MIN;
    g_sss.stk_Pointer = (APTR)g_sss.stk_Upper;
    g_argc = argc;
    g_argv = argv;
    StackSwap(&g_sss);
    daemon_on_own_stack();
    StackSwap(&g_sss);
    FreeMem(g_sss.stk_Lower, STACK_MIN);
    return g_rc;
}
