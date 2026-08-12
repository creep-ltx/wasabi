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
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <proto/bsdsocket.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define VERSION_STR "wasabid 0.1b21"
/* 'used' so the optimizer cannot drop it - C:Version reads this string. */
static const char *verstag __attribute__((used)) =
    "$VER: wasabid 0.1b21 (12.8.2026)";

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
                 "reboot,restart,ps,kill,speed,speedfile,quit,install"
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

struct Library *SocketBase;

/* --- the one running command -------------------------------------- */

/*
 * Only one RUN may be in flight at a time. That is a deliberate limit,
 * not an oversight: it makes the handshake with the runner process
 * unambiguous (one global, no queue) and a developer driving one Amiga
 * has no use for two concurrent builds. A second RUN gets a clear error.
 */
struct RunJob {
    char             cmd[512];
    char             outname[64];
    volatile LONG    rc;
    volatile LONG    ioerr;
    volatile BOOL    done;
    volatile BOOL    taken;
    struct Task     *owner;
    ULONG            sigmask;
};

static struct RunJob  g_job;
static struct RunJob *g_handoff;     /* parent -> runner, one at a time */
static int            g_run_client = -1;
static BPTR           g_run_read;    /* our read end of the temp file */
static LONG           g_run_sent;    /* bytes already forwarded */

/*
 * The debug stream, standalone: wasabid patches exec's RawPutChar (LVO
 * -516) itself and captures every KPrintF/serial-debug byte into a ring,
 * which the main loop drains to LOG frames. No Sashimi, no third-party
 * tool - the approach is modelled on Sashimi's public-domain source
 * (Olaf Barthel), which does the same SetFunction trick.
 *
 * The patch runs in ANY context - task, interrupt, Supervisor - because
 * that is where RawPutChar is called from. So the producer allocates
 * nothing, takes no locks beyond a short Disable(), does no I/O, and
 * (like Sashimi) does NOT chain to the original: serial output is
 * intercepted, not duplicated. Globals are absolute-addressed (no
 * -fbaserel), so the handler needs no a4 setup to reach the ring.
 */
#define DBG_RING  32768              /* power of two, for the & mask */

static volatile UBYTE g_ring[DBG_RING];
static volatile ULONG g_ring_head;   /* producer (patch) advances */
static volatile ULONG g_ring_tail;   /* consumer (daemon) advances */
static volatile ULONG g_ring_lost;   /* bytes dropped on a full ring */

static int   g_dbg_client = -1;
static ULONG g_dbg_seq;
static BOOL  g_dbg_patched;          /* our RawPutChar patch is installed */
static APTR  g_orig_rawput;          /* the vector we replaced */

/*
 * The SetFunction trampoline, in a top-level asm block so it is a real
 * linkable symbol. Entry: D0.b = char, A6 = ExecBase. Save the exec
 * scratch registers, hand the char to the C store on the stack, restore,
 * return. We do not call the original - matching Sashimi.
 */
extern void wasabi_rawput_patch(void);
void wasabi_store(UBYTE c);          /* forward decl; defined below */

asm(
    "    .text                       \n"
    "    .even                       \n"
    "    .globl _wasabi_rawput_patch \n"
    "_wasabi_rawput_patch:           \n"
    "    movem.l %d0-%d1/%a0-%a1,-(%sp) \n"
    "    move.l  %d0,-(%sp)          \n"   /* char as an int arg */
    "    jsr     _wasabi_store       \n"
    "    addq.l  #4,%sp              \n"
    "    movem.l (%sp)+,%d0-%d1/%a0-%a1 \n"
    "    rts                         \n"
);

/* Producer. Runs in arbitrary context - keep it tiny and lock-light. */
void wasabi_store(UBYTE c)
{
    ULONG next;
    if (c == '\0')
        return;                      /* KPrintF pads with NULs; skip them */
    Disable();
    next = (g_ring_head + 1) & (DBG_RING - 1);
    if (next == g_ring_tail)
        g_ring_lost++;               /* full: drop, count it */
    else {
        g_ring[g_ring_head] = c;
        g_ring_head = next;
    }
    Enable();
}

struct Client {
    int   fd;
    BOOL  hello;
};

static struct Client g_clients[MAX_CLIENTS];
static char g_key[128];
static BOOL g_quit;
static BOOL g_restart;               /* relaunch ourselves on the way out */
static int  g_port = DEF_PORT;        /* the port we listen on */
static char g_extra_args[128];       /* replayed on restart, so a running
                                      * allow-list survives a self-update */

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
static LONG  g_refused_written;      /* ds_Minute of the last file write */

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

/* Push a string into the debug ring, so refusals arrive on `wasabi
 * debug` in order with everything else and need no second channel. */
static void dbg_say(const char *s)
{
    while (*s)
        wasabi_store((UBYTE)*s++);
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
    dbg_say(line);
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
    dbg_say(line);
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

static BOOL send_all(int fd, const UBYTE *buf, LONG len)
{
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
    hdr[0] = tag;
    put_be32(hdr + 1, (ULONG)len);
    if (!send_all(fd, hdr, 5))
        return FALSE;
    return len ? send_all(fd, (const UBYTE *)payload, len) : TRUE;
}

static BOOL recv_all(int fd, UBYTE *buf, LONG len)
{
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

static BOOL send_err(int fd, const char *msg)
{
    UBYTE buf[256];
    LONG mlen = (LONG)strlen(msg);
    if (mlen > 200) mlen = 200;
    put_be32(buf, (ULONG)IoErr());
    buf[4] = (UBYTE)(mlen >> 8);
    buf[5] = (UBYTE)mlen;
    memcpy(buf + 6, msg, mlen);
    return send_frame(fd, T_ERR, buf, 6 + mlen);
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

    job->done = TRUE;
    if (job->owner)
        Signal(job->owner, job->sigmask);
}

static BOOL start_run(int cl, const char *cmd)
{
    static LONG serial;
    struct Process *proc;

    memset(&g_job, 0, sizeof(g_job));
    strncpy(g_job.cmd, cmd, sizeof(g_job.cmd) - 1);
    sprintf(g_job.outname, "T:wasabi-run-%ld", (long)++serial);
    g_job.owner   = FindTask(NULL);
    g_job.sigmask = SIGBREAKF_CTRL_F;

    /* Create the file up front so our own tail can open it immediately. */
    {
        BPTR seed = Open(g_job.outname, MODE_NEWFILE);
        if (!seed)
            return FALSE;
        Close(seed);
    }

    g_handoff = &g_job;
    proc = CreateNewProcTags(NP_Entry,     (ULONG)runner_entry,
                             NP_Name,      (ULONG)"wasabi-runner",
                             NP_StackSize, 16384,
                             NP_Cli,       TRUE,
                             TAG_DONE);
    if (!proc) {
        g_handoff = NULL;
        DeleteFile(g_job.outname);
        return FALSE;
    }
    /* Wait for the runner to pick the job up before reusing the global. */
    while (!g_job.taken)
        Delay(1);
    g_handoff = NULL;

    g_run_read = Open(g_job.outname, MODE_OLDFILE);
    g_run_sent = 0;
    g_run_client = cl;
    return TRUE;
}

/* Forward whatever the child has flushed. Returns FALSE if the client died. */
static BOOL pump_run(void)
{
    UBYTE buf[RUNBUF];
    int fd = g_clients[g_run_client].fd;
    LONG n;

    if (g_run_read) {
        while ((n = Read(g_run_read, buf, sizeof(buf))) > 0) {
            g_run_sent += n;
            if (!send_frame(fd, T_STDOUT, buf, n))
                return FALSE;
        }
    }
    if (g_job.done) {
        UBYTE ex[8];
        /* One last sweep: the child may have flushed as it exited. */
        if (g_run_read) {
            while ((n = Read(g_run_read, buf, sizeof(buf))) > 0)
                if (!send_frame(fd, T_STDOUT, buf, n))
                    return FALSE;
            Close(g_run_read);
            g_run_read = 0;
        }
        DeleteFile(g_job.outname);
        put_be32(ex, (ULONG)g_job.rc);
        put_be32(ex + 4, (ULONG)g_job.ioerr);
        g_run_client = -1;
        return send_frame(fd, T_EXIT, ex, 8);
    }
    return TRUE;
}

/* --- the debug stream ---------------------------------------------- */

static BOOL send_log(int fd, ULONG stream, ULONG seq,
                     const UBYTE *text, LONG len)
{
    UBYTE pl[10 + RUNBUF];
    if (len > RUNBUF)
        len = RUNBUF;
    put_be32(pl, stream);
    put_be32(pl + 4, seq);
    pl[8] = (UBYTE)(len >> 8);
    pl[9] = (UBYTE)len;
    memcpy(pl + 10, text, len);
    return send_frame(fd, T_LOG, pl, 10 + len);
}

static void debug_install(void)
{
    if (g_dbg_patched)
        return;
    /* Disable across SetFunction: RawPutChar can fire from interrupts. */
    Disable();
    g_orig_rawput = SetFunction((struct Library *)SysBase, -516,
                                (ULONG (*)())wasabi_rawput_patch);
    Enable();
    g_dbg_patched = TRUE;
}

/*
 * Remove the patch, but only if the vector still points at ours. If
 * someone SetFunction'd on top of us, restoring the old pointer would
 * unlink THEIR patch and crash the machine later - so we put ours back
 * and stay installed rather than corrupt the chain. (Same rule Sashimi
 * uses; the honest failure is a patch that will not leave, not a Guru
 * with no traceable cause.)
 */
static BOOL debug_uninstall(void)
{
    APTR res;
    BOOL removed;
    if (!g_dbg_patched)
        return TRUE;
    Disable();
    res = SetFunction((struct Library *)SysBase, -516,
                      (ULONG (*)())g_orig_rawput);
    if (res == (APTR)wasabi_rawput_patch) {
        removed = TRUE;
    } else {
        /* Someone is on top of us - undo our restore, leave the stack. */
        SetFunction((struct Library *)SysBase, -516, (ULONG (*)())res);
        removed = FALSE;
    }
    Enable();
    if (removed)
        g_dbg_patched = FALSE;
    return removed;
}

/* Consumer side: pull bytes the patch has queued. Single consumer, so no
 * Disable needed - we only read head and advance tail. */
static LONG ring_drain(UBYTE *buf, LONG max)
{
    ULONG head = g_ring_head;            /* one atomic snapshot */
    LONG n = 0;
    while (n < max && g_ring_tail != head) {
        buf[n++] = g_ring[g_ring_tail];
        g_ring_tail = (g_ring_tail + 1) & (DBG_RING - 1);
    }
    return n;
}

static BOOL debug_start(int cl)
{
    if (g_dbg_client >= 0)
        return send_err(g_clients[cl].fd, "the debug stream is already in use");

    g_ring_head = g_ring_tail = g_ring_lost = 0;
    g_dbg_client = cl;
    g_dbg_seq = 0;
    debug_install();
    return TRUE;                          /* LOG frames follow from the pump */
}

static BOOL debug_pump(void)
{
    UBYTE buf[RUNBUF];
    int fd = g_clients[g_dbg_client].fd;
    LONG n;

    while ((n = ring_drain(buf, sizeof(buf))) > 0)
        if (!send_log(fd, 0, ++g_dbg_seq, buf, n))
            return FALSE;

    if (g_ring_lost) {
        char note[64];
        LONG ln = sprintf(note, "\n[wasabi: %lu debug byte(s) lost]\n",
                          (unsigned long)g_ring_lost);
        g_ring_lost = 0;
        if (!send_log(fd, 0, ++g_dbg_seq, (UBYTE *)note, ln))
            return FALSE;
    }
    return TRUE;
}

static void debug_stop(void)
{
    debug_uninstall();
    g_dbg_client = -1;
}

/* --- the snoop stream ---------------------------------------------- */

/*
 * The SnoopDOS trick, after Eddy Carroll's public source: SetFunction()
 * patches on the dos.library and exec.library calls a developer most
 * wants to see. Unlike SnoopDOS we log AFTER the original returns, so
 * every line carries the real result and IoErr() instead of "pending".
 *
 * Each patch is a two-instruction stub that pushes its descriptor and
 * jumps to one common trampoline. The trampoline runs in the CALLER's
 * context - any task on the machine - so the same rules as the debug
 * patch apply, plus one more: nothing reachable from here may call a
 * function we patch, or it recurses without bound. The C side therefore
 * reads SysBase->ThisTask directly and only ever copies memory.
 *
 * Three ideas are borrowed straight from SnoopDOS's patchcode.s:
 *  - the stub tests an enabled flag first and falls through to the
 *    original untouched when snooping is off, so a patch that cannot be
 *    removed (someone chained onto it) idles at a few instructions;
 *  - a use count is bumped while a caller is inside the stub, so the
 *    daemon can wait for stragglers before its code segment unloads;
 *  - events are dropped, and counted, when the calling task is low on
 *    stack rather than overflowing it building the record.
 */

#define SN_EVMAX   512               /* events in the ring, power of two.
                                      * ~107 KB of BSS: a PiStorm-fast CPU
                                      * can fire >150 patched calls between
                                      * two 50 ms drains (List SYS:C did),
                                      * and 64 dropped most of that burst */
#define SN_NONE    0xFF              /* "no such argument" register index */

/* Register-file indices: the trampoline saves d0-d7/a0-a6 in order. */
#define RF_D0 0
#define RF_D1 1
#define RF_D2 2
#define RF_D3 3
#define RF_A0 8
#define RF_A1 9

#define SN_DOS  0                    /* which library the LVO lives in */
#define SN_EXEC 1

#define NK_NONE     0                /* how to render the numeric arg */
#define NK_OPENMODE 1
#define NK_LOCKMODE 2
#define NK_VERSION  3
#define NK_UNIT     4

#define RK_BOOL 0                    /* how to read d0: nonzero = ok */
#define RK_RC   1                    /* a return code (SystemTagList) */
#define RK_LEN  2                    /* -1 = fail, else a length (GetVar) */
#define RK_ZERO 3                    /* zero = ok (OpenDevice) */

#define SNF_SOFT2 1                  /* str2 is a string only if d3 != 0 */

struct SnoopFn {
    APTR        orig;                /* MUST be first: the asm reads (a2) */
    const char *name;
    APTR        stub;
    UBYTE       lib;                 /* SN_DOS / SN_EXEC */
    WORD        lvo;
    UBYTE       str1, str2;          /* register-file index of string args */
    UBYTE       numreg;              /* index of the numeric arg */
    UBYTE       nkind, rkind, flags;
    UBYTE       installed;
};

struct SnoopEv {
    struct SnoopFn *fn;
    char  task[32];
    char  s1[100];
    char  s2[64];
    BOOL  has2;
    LONG  num;
    LONG  res;
    LONG  err;                       /* pr_Result2 after the call */
};

static struct SnoopEv g_snev[SN_EVMAX];
static volatile ULONG g_snev_head;   /* producer (patches) advances */
static volatile ULONG g_snev_tail;   /* consumer (daemon) advances */
static volatile ULONG g_snev_lost;

static int          g_snoop_client = -1;
static ULONG        g_snoop_seq;
static char         g_snoop_pat[64];
static struct Task *g_snoop_self;    /* the daemon; its own calls are not
                                      * logged, or draining the ring over
                                      * bsdsocket could feed the ring */

/* Read by the trampoline, so plain globals with asm-visible names. */
volatile UWORD wasabi_snoop_on;      /* stubs record only while set */
volatile UWORD wasabi_snoop_users;   /* callers currently inside a stub */

void snoop_record(struct SnoopFn *d, ULONG *regs, LONG res);

/*
 * HERE BE DRAGONS. This trampoline and the RF_* indices in the
 * descriptors above are one definition split in two: the movem below
 * decides what the 60-byte register file looks like, and the indices
 * decide where an argument is read from inside it. No compiler checks
 * that they agree. Get it wrong and snoop_copystr() dereferences
 * whatever was in the wrong register, in another task's context, on a
 * machine with no memory protection.
 *
 * Change nothing here without running snoop_selftest() on real
 * hardware - it exists precisely to catch this, and `wasabi snoop`
 * runs it before every session.
 *
 * The common trampoline. Entered from a per-function stub that has just
 * pushed its descriptor, so the stack is [desc][caller RA] and every
 * argument register still holds the caller's value.
 *
 * Off: put the original's address where the descriptor was and rts into
 * it, clobbering nothing (the a0 juggle is because 68000 has no
 * memory-to-memory move through a pointer).
 *
 * On: bump the use count, save the full register file, call the original
 * with the caller's registers intact, hand descriptor + register file +
 * result to C, then restore with d0 (and d1 - old dos.library returned
 * results in both, and SnoopDOS keeps that quirk alive) as the result.
 */
asm(
    "    .text                          \n"
    "    .even                          \n"
    "    .globl _wasabi_snoop_common    \n"
    "_wasabi_snoop_common:              \n"
    "    tst.w   _wasabi_snoop_on       \n"
    "    bne     snoop_live             \n"
    "    move.l  %a0,-(%sp)             \n"  /* [a0][desc][RA] */
    "    move.l  4(%sp),%a0             \n"  /* a0 = desc */
    "    move.l  (%a0),%a0              \n"  /* a0 = desc->orig */
    "    move.l  %a0,4(%sp)             \n"  /* [a0][orig][RA] */
    "    move.l  (%sp)+,%a0             \n"  /* [orig][RA] */
    "    rts                            \n"  /* jump to the original */
    "snoop_live:                        \n"
    "    addq.w  #1,_wasabi_snoop_users \n"
    "    movem.l %d0-%d7/%a0-%a6,-(%sp) \n"  /* the 60-byte register file */
    "    move.l  60(%sp),%a2            \n"  /* a2 = desc */
    "    move.l  (%a2),%a3              \n"  /* a3 = desc->orig */
    "    jsr     (%a3)                  \n"  /* argument regs untouched */
    "    move.l  %d0,-(%sp)             \n"  /* C arg 3: the result */
    "    pea     4(%sp)                 \n"  /* C arg 2: the register file */
    "    move.l  %a2,-(%sp)             \n"  /* C arg 1: the descriptor */
    "    jsr     _snoop_record          \n"
    "    addq.l  #8,%sp                 \n"
    "    move.l  (%sp)+,%d0             \n"  /* result back... */
    "    move.l  %d0,(%sp)              \n"  /* ...into the file's d0 slot */
    "    movem.l (%sp)+,%d0-%d7/%a0-%a6 \n"
    "    addq.l  #4,%sp                 \n"  /* drop the descriptor */
    "    subq.w  #1,_wasabi_snoop_users \n"
    "    move.l  %d0,%d1                \n"
    "    rts                            \n"
);

#define SNOOP_DESC(nm, lib, lvo, s1, s2, nreg, nk, rk, fl)                 \
extern void snoop_stub_##nm(void);                                         \
asm("    .text                          \n"                                \
    "    .even                          \n"                                \
    "    .globl _snoop_stub_" #nm "     \n"                                \
    "_snoop_stub_" #nm ":               \n"                                \
    "    pea     _snoop_desc_" #nm "    \n"                                \
    "    jmp     _wasabi_snoop_common   \n");                              \
struct SnoopFn snoop_desc_##nm = {                                         \
    NULL, #nm, (APTR)snoop_stub_##nm, lib, lvo, s1, s2, nreg, nk, rk, fl, \
    FALSE };

/* LVOs verified against the NDK's dos_lib.fd / exec_lib.fd. */
SNOOP_DESC(Open,          SN_DOS,  -30,  RF_D1, SN_NONE, RF_D2,
           NK_OPENMODE, RK_BOOL, 0)
SNOOP_DESC(Lock,          SN_DOS,  -84,  RF_D1, SN_NONE, RF_D2,
           NK_LOCKMODE, RK_BOOL, 0)
SNOOP_DESC(LoadSeg,       SN_DOS,  -150, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(Execute,       SN_DOS,  -222, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(SystemTagList, SN_DOS,  -606, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_RC, 0)
SNOOP_DESC(GetVar,        SN_DOS,  -906, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_LEN, 0)
SNOOP_DESC(SetVar,        SN_DOS,  -900, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(DeleteFile,    SN_DOS,  -72,  RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(Rename,        SN_DOS,  -78,  RF_D1, RF_D2, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(CreateDir,     SN_DOS,  -120, RF_D1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)
SNOOP_DESC(MakeLink,      SN_DOS,  -444, RF_D1, RF_D2, SN_NONE,
           NK_NONE, RK_BOOL, SNF_SOFT2)
SNOOP_DESC(OpenLibrary,   SN_EXEC, -552, RF_A1, SN_NONE, RF_D0,
           NK_VERSION, RK_BOOL, 0)
SNOOP_DESC(OpenDevice,    SN_EXEC, -444, RF_A0, SN_NONE, RF_D0,
           NK_UNIT, RK_ZERO, 0)
SNOOP_DESC(FindPort,      SN_EXEC, -390, RF_A1, SN_NONE, SN_NONE,
           NK_NONE, RK_BOOL, 0)

static struct SnoopFn *snoop_fns[] = {
    &snoop_desc_Open,        &snoop_desc_Lock,      &snoop_desc_LoadSeg,
    &snoop_desc_Execute,     &snoop_desc_SystemTagList,
    &snoop_desc_GetVar,      &snoop_desc_SetVar,    &snoop_desc_DeleteFile,
    &snoop_desc_Rename,      &snoop_desc_CreateDir, &snoop_desc_MakeLink,
    &snoop_desc_OpenLibrary, &snoop_desc_OpenDevice, &snoop_desc_FindPort,
};
#define SN_NFN (LONG)(sizeof(snoop_fns) / sizeof(snoop_fns[0]))

/* strncpy that always terminates and tolerates a NULL source. */
static void snoop_copystr(char *dst, LONG dstsz, const char *src)
{
    LONG i = 0;
    if (!src) src = "(null)";
    while (i < dstsz - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* The expensive half, in its own frame so the headroom check in
 * snoop_record() has already passed before this local exists. */
static void __attribute__((noinline))
snoop_record2(struct SnoopFn *d, ULONG *regs, LONG res)
{
    struct SnoopEv ev;
    struct Task *t = SysBase->ThisTask;
    ULONG next;

    ev.fn   = d;
    ev.num  = (d->numreg != SN_NONE) ? (LONG)regs[d->numreg] : 0;
    ev.res  = res;
    ev.err  = 0;
    ev.has2 = FALSE;
    ev.task[0] = '\0';

    /* Name the culprit the way SnoopDOS does: the CLI command being run
     * if there is one, else the task name. */
    if (t->tc_Node.ln_Type == NT_PROCESS) {
        struct Process *pr = (struct Process *)t;
        struct CommandLineInterface *cli =
            (struct CommandLineInterface *)BADDR(pr->pr_CLI);
        ev.err = pr->pr_Result2;
        if (cli) {
            UBYTE *b = (UBYTE *)BADDR(cli->cli_CommandName);
            if (b && b[0]) {                 /* a BSTR: length, then bytes */
                LONG n = b[0];
                if (n > (LONG)sizeof(ev.task) - 1)
                    n = sizeof(ev.task) - 1;
                memcpy(ev.task, b + 1, n);
                ev.task[n] = '\0';
            }
        }
    }
    if (!ev.task[0])
        snoop_copystr(ev.task, sizeof(ev.task), t->tc_Node.ln_Name);

    if (d->str1 != SN_NONE)
        snoop_copystr(ev.s1, sizeof(ev.s1), (const char *)regs[d->str1]);
    else
        ev.s1[0] = '\0';
    if (d->str2 != SN_NONE) {
        ev.has2 = TRUE;
        if ((d->flags & SNF_SOFT2) && !regs[RF_D3])
            strcpy(ev.s2, "(hard link to a lock)");
        else
            snoop_copystr(ev.s2, sizeof(ev.s2), (const char *)regs[d->str2]);
    } else
        ev.s2[0] = '\0';

    Disable();
    next = (g_snev_head + 1) & (SN_EVMAX - 1);
    if (next == g_snev_tail)
        g_snev_lost++;                       /* full: drop, count it */
    else {
        g_snev[g_snev_head] = ev;
        g_snev_head = next;
    }
    Enable();
}

/*
 * Producer half, called from the trampoline in the context of whatever
 * task made the call - after the original has already returned.
 */
void snoop_record(struct SnoopFn *d, ULONG *regs, LONG res)
{
    struct Task *t = SysBase->ThisTask;
    char probe;

    if (!wasabi_snoop_on || t == g_snoop_self)
        return;
    /* SnoopDOS's stack rule: if sp is inside the task's declared stack,
     * insist on headroom before building the event on it; an sp outside
     * the bounds (CLI programs swap stacks) is assumed to be roomy. */
    if ((APTR)&probe > t->tc_SPLower && (APTR)&probe <= t->tc_SPUpper &&
        (char *)&probe - (char *)t->tc_SPLower < 800) {
        Disable(); g_snev_lost++; Enable();
        return;
    }
    snoop_record2(d, regs, res);
}

static struct Library *snoop_base(struct SnoopFn *d)
{
    return d->lib == SN_EXEC ? (struct Library *)SysBase
                             : (struct Library *)DOSBase;
}

static void snoop_install(void)
{
    LONG i;
    for (i = 0; i < SN_NFN; i++) {
        struct SnoopFn *d = snoop_fns[i];
        if (d->installed)
            continue;                /* left over from a stuck teardown */
        Disable();
        d->orig = (APTR)SetFunction(snoop_base(d), d->lvo,
                                    (ULONG (*)())d->stub);
        Enable();
        d->installed = TRUE;
    }
}

/* Reverse order, same chain rule as the debug patch: if the vector no
 * longer points at us, put the interloper back and stay installed. */
static BOOL snoop_uninstall(void)
{
    LONG i;
    BOOL all = TRUE;
    for (i = SN_NFN - 1; i >= 0; i--) {
        struct SnoopFn *d = snoop_fns[i];
        APTR res;
        if (!d->installed)
            continue;
        Disable();
        res = (APTR)SetFunction(snoop_base(d), d->lvo, (ULONG (*)())d->orig);
        if (res == d->stub)
            d->installed = FALSE;
        else {
            SetFunction(snoop_base(d), d->lvo, (ULONG (*)())res);
            all = FALSE;             /* the stub stays live; its enabled
                                      * check makes it a cheap no-op */
        }
        Enable();
    }
    return all;
}

static BOOL snoop_stuck(void)
{
    LONG i;
    for (i = 0; i < SN_NFN; i++)
        if (snoop_fns[i]->installed)
            return TRUE;
    return FALSE;
}

/*
 * Case-insensitive match with the AmigaDOS '#?' and '?' wildcards ('*'
 * too, since fingers type it). Not full ParsePattern - but this runs on
 * task names, not paths, and needs no dos.library call.
 */
static BOOL pat_match(const char *pat, const char *s)
{
    while (*pat) {
        if ((pat[0] == '#' && pat[1] == '?') || pat[0] == '*') {
            const char *rest = pat + (pat[0] == '*' ? 1 : 2);
            for (;; s++) {
                if (pat_match(rest, s))
                    return TRUE;
                if (!*s)
                    return FALSE;
            }
        }
        if (!*s)
            return FALSE;
        if (pat[0] != '?' &&
            tolower((unsigned char)pat[0]) != tolower((unsigned char)s[0]))
            return FALSE;
        pat++; s++;
    }
    return *s == '\0';
}

/* Daemon context from here down. */

static LONG snoop_format(struct SnoopEv *ev, char *out)
{
    struct SnoopFn *d = ev->fn;
    LONG n = sprintf(out, "%-20s %s(", ev->task, d->name);

    if (d->str1 != SN_NONE)
        n += sprintf(out + n, "\"%s\"", ev->s1);
    if (ev->has2)
        n += sprintf(out + n, ", \"%s\"", ev->s2);

    switch (d->nkind) {
    case NK_OPENMODE:
        if (ev->num == 1005)      n += sprintf(out + n, ", read");
        else if (ev->num == 1006) n += sprintf(out + n, ", create");
        else if (ev->num == 1004) n += sprintf(out + n, ", readwrite");
        else n += sprintf(out + n, ", mode %ld", (long)ev->num);
        break;
    case NK_LOCKMODE:
        if (ev->num == -2)        n += sprintf(out + n, ", read");
        else if (ev->num == -1)   n += sprintf(out + n, ", write");
        else n += sprintf(out + n, ", type %ld", (long)ev->num);
        break;
    case NK_VERSION:
        n += sprintf(out + n, ", v%ld", (long)ev->num);
        break;
    case NK_UNIT:
        n += sprintf(out + n, ", unit %ld", (long)ev->num);
        break;
    }

    switch (d->rkind) {
    case RK_RC:
        n += sprintf(out + n, ") = rc %ld\n", (long)ev->res);
        break;
    case RK_LEN:
        if (ev->res >= 0)
            n += sprintf(out + n, ") = %ld byte(s)\n", (long)ev->res);
        else if (ev->err)
            n += sprintf(out + n, ") = fail (err %ld)\n", (long)ev->err);
        else
            n += sprintf(out + n, ") = fail\n");
        break;
    case RK_ZERO:
        if (ev->res == 0)
            n += sprintf(out + n, ") = ok\n");
        else
            n += sprintf(out + n, ") = error %ld\n", (long)ev->res);
        break;
    default:                         /* RK_BOOL */
        if (ev->res)
            n += sprintf(out + n, ") = ok\n");
        else if (ev->err && d->lib == SN_DOS)
            n += sprintf(out + n, ") = fail (err %ld)\n", (long)ev->err);
        else
            n += sprintf(out + n, ") = fail\n");
        break;
    }
    return n;
}

/*
 * Prove the trampoline before trusting it.
 *
 * The asm hands snoop_record() a 60-byte register file; the RF_* indices
 * in each descriptor say where in it an argument lives. Those are two
 * definitions that must agree exactly, and nothing at compile time
 * checks that they do. The failure mode is not a wrong log line - it is
 * a wild pointer copied out of the wrong register and dereferenced by
 * snoop_copystr(), on a machine with no MMU, in the context of whatever
 * task happened to call Lock().
 *
 * So before any of that can happen: un-exclude our own calls, Lock() a
 * bogus path, and insist the captured event carries back that exact
 * string (d1), the mode we passed (d2) and the result the caller
 * actually got (d0). A mismatch means the asm and the C disagree on
 * THIS build, and snoop refuses to run rather than corrupt memory three
 * minutes later somewhere that looks nothing like the cause.
 *
 * The path is relative - no device lookup, so a missing object cannot
 * raise a "please insert volume" requester - and pr_WindowPtr is -1 for
 * the duration in case anything else would try.
 */
static BOOL snoop_selftest(char *why, LONG whysz)
{
    static LONG serial;
    char path[64];
    struct Task *self = FindTask(NULL);
    struct Process *me = (struct Process *)self;
    BOOL isproc = self->tc_Node.ln_Type == NT_PROCESS;
    APTR oldwin = NULL;
    BPTR lock;
    ULONG i;
    BOOL found = FALSE;

    /* Unique per attempt, so a stale ring entry can never be mistaken
     * for this run's event. */
    sprintf(path, "wasabi-selftest-%ld-%lx", (long)++serial,
            (unsigned long)self);

    g_snev_head = g_snev_tail = g_snev_lost = 0;
    g_snoop_self = NULL;             /* record our own call, just this once */
    wasabi_snoop_on = 1;

    if (isproc) {
        oldwin = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;
    }
    lock = Lock(path, ACCESS_READ);  /* must fail; nothing to unlock */
    if (isproc)
        me->pr_WindowPtr = oldwin;

    wasabi_snoop_on = 0;
    g_snoop_self = self;

    if (lock)                        /* absurd, but do not leak it */
        UnLock(lock);

    for (i = g_snev_tail; i != g_snev_head; i = (i + 1) & (SN_EVMAX - 1)) {
        struct SnoopEv *ev = &g_snev[i];
        if (ev->fn != &snoop_desc_Lock)
            continue;                /* another task's call, in the window */
        if (strcmp(ev->s1, path) != 0) {
            snoop_copystr(why, whysz, "the path argument came back wrong");
            goto done;
        }
        if (ev->num != ACCESS_READ) {
            snoop_copystr(why, whysz, "the mode argument came back wrong");
            goto done;
        }
        if (ev->res != (LONG)lock) {
            snoop_copystr(why, whysz, "the result came back wrong");
            goto done;
        }
        found = TRUE;
        snoop_copystr(why, whysz, "ok");
        break;
    }
    if (!found)
        snoop_copystr(why, whysz, g_snev_lost ? "the ring overflowed"
                                              : "the patch captured nothing");
done:
    g_snev_head = g_snev_tail = g_snev_lost = 0;
    return found;
}

static BOOL snoop_start(int cl, const char *pat)
{
    char why[64];

    if (g_snoop_client >= 0)
        return send_err(g_clients[cl].fd, "the snoop stream is already in use");

    snoop_copystr(g_snoop_pat, sizeof(g_snoop_pat), pat);
    g_snev_head = g_snev_tail = g_snev_lost = 0;
    g_snoop_seq = 0;
    g_snoop_self = FindTask(NULL);
    snoop_install();

    /* Patches are live from here; nothing records until the flag is set. */
    if (!snoop_selftest(why, sizeof(why))) {
        char msg[200];
        snoop_uninstall();
        sprintf(msg, "snoop self-test failed (%.60s) - the patch trampoline "
                     "and this build disagree, so snoop will not run", why);
        return send_err(g_clients[cl].fd, msg);
    }

    g_snoop_client = cl;
    wasabi_snoop_on = 1;
    return TRUE;                     /* LOG frames follow from the pump */
}

static BOOL snoop_pump(void)
{
    int fd = g_clients[g_snoop_client].fd;
    char line[320];

    while (g_snev_tail != g_snev_head) {
        struct SnoopEv *ev = &g_snev[g_snev_tail];
        if (!g_snoop_pat[0] || pat_match(g_snoop_pat, ev->task)) {
            LONG n = snoop_format(ev, line);
            if (!send_log(fd, 1, ++g_snoop_seq, (UBYTE *)line, n))
                return FALSE;
        }
        g_snev_tail = (g_snev_tail + 1) & (SN_EVMAX - 1);
    }
    if (g_snev_lost) {
        char note[64];
        LONG ln = sprintf(note, "[wasabi: %lu snoop event(s) lost]\n",
                          (unsigned long)g_snev_lost);
        g_snev_lost = 0;
        if (!send_log(fd, 1, ++g_snoop_seq, (UBYTE *)note, ln))
            return FALSE;
    }
    return TRUE;
}

static void snoop_stop(void)
{
    wasabi_snoop_on = 0;
    snoop_uninstall();               /* best effort; a stuck stub idles */
    g_snoop_client = -1;
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
    LONG  cli;                       /* CLI number, -1 when none */
    char  name[48];
    char  cmd[48];                   /* CLI command in flight, if any */
};

#define PS_MAX 128

static struct PsEnt g_ps[PS_MAX];

static void ps_add(LONG *n, struct Task *t, UBYTE state)
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
    e->cli   = -1;
    e->cmd[0] = '\0';
    snoop_copystr(e->name, sizeof(e->name), t->tc_Node.ln_Name);
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
    Disable();
    ps_add(&n, SysBase->ThisTask, 0);
    for (t = (struct Task *)SysBase->TaskReady.lh_Head;
         t->tc_Node.ln_Succ; t = (struct Task *)t->tc_Node.ln_Succ)
        ps_add(&n, t, 1);
    for (t = (struct Task *)SysBase->TaskWait.lh_Head;
         t->tc_Node.ln_Succ; t = (struct Task *)t->tc_Node.ln_Succ)
        ps_add(&n, t, 2);
    Enable();
    return n;
}

static BOOL cmd_ps(int fd)
{
    static const char * const statename[] = { "run", "ready", "wait" };
    char line[160];
    LONG n = ps_collect(), i;

    for (i = 0; i < n; i++) {
        struct PsEnt *e = &g_ps[i];
        LONG ln = sprintf(line, "0x%08lx %c %ld %s %lu %ld %s\t%s\n",
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
        return send_err(fd, "no task or process by that name");
    if (matches > 1)
        return send_err(fd,
            "ambiguous - several tasks match; use the 0x address from ps");
    if (hit == FindTask(NULL))
        return send_err(fd, "that is wasabid itself - use restart or reboot");

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
            if (flags & 1)
                RemTask(hit);
            else
                Signal(hit, SIGBREAKF_CTRL_C);
        }
        Enable();
    }
    if (!alive)
        return send_err(fd, "that task is already gone");
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
        "chip free %ld KB, fast free %ld KB\n"
        "volumes:\n",
        VERSION_STR, PROTO_VERSION,
        (long)SysBase->LibNode.lib_Version, (long)SysBase->LibNode.lib_Revision,
        (long)(AvailMem(MEMF_CHIP) >> 10), (long)(AvailMem(MEMF_FAST) >> 10));
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
        return send_err(fd, "out of memory");
    }
    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        return send_err(fd, "Examine failed");
    }
    while (ExNext(lock, fib)) {
        LONG n = sprintf(line, "%c %lu %ld %ld %ld %ld %s\n",
                         fib->fib_DirEntryType > 0 ? 'd' : 'f',
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

static BOOL cmd_get(int fd, const char *path)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    UBYTE *buf;
    LONG n;

    if (!fh)
        return send_err(fd, "cannot open for reading");
    buf = AllocMem(MAX_PAYLOAD, MEMF_ANY);
    if (!buf) {
        Close(fh);
        return send_err(fd, "out of memory");
    }
    while ((n = Read(fh, buf, MAX_PAYLOAD)) > 0) {
        if (!send_frame(fd, T_DATA, buf, n)) {
            FreeMem(buf, MAX_PAYLOAD);
            Close(fh);
            return FALSE;
        }
    }
    FreeMem(buf, MAX_PAYLOAD);
    Close(fh);
    return send_frame(fd, T_END, NULL, 0);
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
static BOOL cmd_put(int fd, ULONG size, ULONG prot, const char *path)
{
    char tmp[300];
    BPTR fh;
    UBYTE *buf;
    ULONG got = 0;
    BOOL ok = TRUE;

    buf = AllocMem(MAX_PAYLOAD, MEMF_ANY);
    if (!buf)
        return send_err(fd, "out of memory");

    if (is_self_file(path)) {
        /* The client is already sending the body; swallow it, or those
         * DATA frames get read back as commands and desync the session. */
        for (;;) {
            UBYTE tag;
            LONG n;
            if (!recv_frame(fd, &tag, buf, &n)) {
                FreeMem(buf, MAX_PAYLOAD);
                return FALSE;
            }
            if (tag == T_END || tag != T_DATA)
                break;
        }
        FreeMem(buf, MAX_PAYLOAD);
        return send_err(fd, "that is the running daemon - use 'wasabi update', "
                            "which verifies the binary before it commits");
    }

    sprintf(tmp, "%.280s.wasabi-tmp", path);
    fh = Open(tmp, MODE_NEWFILE);
    if (!fh) {
        FreeMem(buf, MAX_PAYLOAD);
        return send_err(fd, "cannot create the temporary file");
    }

    for (;;) {
        UBYTE tag;
        LONG len;
        if (!recv_frame(fd, &tag, buf, &len)) { ok = FALSE; break; }
        if (tag == T_END)
            break;
        if (tag != T_DATA) { ok = FALSE; break; }
        if (Write(fh, buf, len) != len) {
            FreeMem(buf, MAX_PAYLOAD);
            Close(fh);
            DeleteFile(tmp);
            return send_err(fd, "write failed - disk full?");
        }
        got += len;
    }
    FreeMem(buf, MAX_PAYLOAD);
    Close(fh);

    if (!ok || got != size) {
        DeleteFile(tmp);
        return ok ? send_err(fd, "size mismatch") : FALSE;
    }
    DeleteFile(path);                    /* Rename won't clobber */
    if (!Rename(tmp, (STRPTR)path)) {
        DeleteFile(tmp);
        return send_err(fd, "rename into place failed");
    }
    if (prot != 0xFFFFFFFFUL)
        SetProtection((STRPTR)path, (LONG)prot);
    return send_frame(fd, T_OK, NULL, 0);
}

/*
 * Throughput measurement, deliberately storage-free: received bytes are
 * counted and dropped, sent bytes come from one static-pattern buffer.
 * Nothing lands in RAM: or on a volume, so a stock 2 MB machine runs
 * the same 50 MB test a PiStorm does, and the number isolates the
 * network path instead of blending in a filesystem.
 */
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
        snoop_copystr(why, whysz, "no such drawer or volume");
        return FALSE;
    }
    if (!Info(lock, &id)) {
        UnLock(lock);
        snoop_copystr(why, whysz, "cannot read the volume's free space");
        return FALSE;
    }
    UnLock(lock);
    vol_megabytes(&id, &total, &freemb);
    if (id.id_DiskState == ID_WRITE_PROTECTED) {
        snoop_copystr(why, whysz, "that volume is write-protected");
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
        return send_err(fd, "size must be 1 byte to 256 MB");
    buf = AllocMem(MAX_PAYLOAD, MEMF_ANY);
    if (!buf)
        return send_err(fd, "out of memory");

    if (tofile) {
        char why[100], msg[200];
        if (!(flags & 1) && !speed_room(target, size, why, sizeof(why))) {
            BOOL alive = speed_drain(fd, buf);
            FreeMem(buf, MAX_PAYLOAD);
            if (!alive)
                return FALSE;
            sprintf(msg, "cannot speedtest to %.40s: %.140s", target, why);
            return send_err(fd, msg);
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
                Close(fh); FreeMem(buf, MAX_PAYLOAD);
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

static BOOL cmd_reboot(int fd, ULONG flags)
{
    (void)flags;
    if (!send_frame(fd, T_OK, NULL, 0))
        return FALSE;
    /* Give the ack a moment to leave the wire, then flush and go. */
    Delay(25);
    CloseSocket(g_clients[0].fd);        /* best effort; we are going down */
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
        return send_err(fd, "cannot tell which path I was started from");

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
static BOOL cmd_quit(int fd)
{
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
static BOOL cmd_restart(int fd)
{
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
    n = sprintf(reply, "WASABI!1 amiga %d %s\n", port, VERSION_STR);
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
            return send_err(fd, "expected HELLO"), FALSE;
        if (len < 2 || get_be16(p) != PROTO_VERSION)
            return send_err(fd, "protocol version mismatch"), FALSE;
        if (!get_str(p, len, 2, key, sizeof(key)))
            return send_err(fd, "malformed HELLO"), FALSE;
        if (strcmp(key, g_key) != 0)
            return send_err(fd, "bad key"), FALSE;
        g_clients[cl].hello = TRUE;
        {
            UBYTE w[256];
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

    switch (tag) {
    case T_PING:
        return send_frame(fd, T_PONG, NULL, 0);

    case T_INFO:
        return cmd_info(fd);

    case T_LS:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_err(fd, "bad path");
        return cmd_ls(fd, path);

    case T_GET:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_err(fd, "bad path");
        return cmd_get(fd, path);

    case T_PUT:
        if (len < 8 || !get_str(p, len, 8, path, sizeof(path)))
            return send_err(fd, "bad PUT header");
        return cmd_put(fd, get_be32(p), get_be32(p + 4), path);

    case T_DEL:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_err(fd, "bad path");
        return DeleteFile(path) ? send_frame(fd, T_OK, NULL, 0)
                                : send_err(fd, "delete failed");

    case T_MKDIR:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_err(fd, "bad path");
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
            return send_err(fd, "bad RUN header");
        if (g_run_client >= 0)
            return send_err(fd, "another command is already running");
        if (!start_run(cl, cmd))
            return send_err(fd, "could not start the command");
        return TRUE;                     /* output follows from pump_run */
    }

    case T_PS:
        return cmd_ps(fd);

    case T_KILL: {
        char target[64];
        if (len < 4 || !get_str(p, len, 4, target, sizeof(target)))
            return send_err(fd, "bad KILL header");
        return cmd_kill(fd, get_be32(p), target);
    }

    case T_SPEED: {
        char target[200];
        if (len < 8)
            return send_err(fd, "bad SPEED header");
        if (len == 8)
            target[0] = '\0';            /* older client: storage-free mode */
        else if (!get_str(p, len, 8, target, sizeof(target)))
            return send_err(fd, "bad SPEED target");
        return cmd_speed(fd, get_be32(p), get_be32(p + 4), target);
    }

    case T_REBOOT:
        return cmd_reboot(fd, len >= 4 ? get_be32(p) : 0);

    case T_RESTART:
        return cmd_restart(fd);

    case T_QUIT:
        return cmd_quit(fd);

    case T_INSTALL:
        if (!get_str(p, len, 0, path, sizeof(path)))
            return send_err(fd, "bad path");
        return cmd_install(fd, path);

    case T_DEBUG:
        return debug_start(cl);

    case T_SNOOP: {
        char pat[64];
        if (len < 4 || !get_str(p, len, 4, pat, sizeof(pat)))
            return send_err(fd, "bad SNOOP header");
        return snoop_start(cl, pat);
    }

    default:
        return send_err(fd, "unknown command");
    }
}

/* --- main ----------------------------------------------------------- */

static void drop(int cl)
{
    if (g_run_client == cl) {            /* the run outlives its client */
        if (g_run_read) { Close(g_run_read); g_run_read = 0; }
        g_run_client = -1;
    }
    if (g_dbg_client == cl)
        debug_stop();
    if (g_snoop_client == cl)
        snoop_stop();
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

int main(int argc, char **argv)
{
    int listen_fd = -1, disco_fd = -1, port = DEF_PORT, i;
    UBYTE *payload;

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return selftest(argc > 2 ? argv[2] : "-");

    /*
     * Arguments, in any order:
     *   <port>          listen somewhere else - lets a trial instance run
     *                   beside a live one without a bind clash
     *   allow <cidr>    also answer this range, e.g. a Tailscale 100.64/10
     *   allow any       answer anybody at all; see the warning below
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "allow") == 0 && i + 1 < argc) {
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
        if (g_allow_any)
            sprintf(g_extra_args + n, " allow any");
    }

    /* The daemon has no console either: a requester raised by ls, get or
     * put would hang the whole loop, not just one command. */
    {
        struct Task *me = FindTask(NULL);
        if (me->tc_Node.ln_Type == NT_PROCESS)
            ((struct Process *)me)->pr_WindowPtr = (APTR)-1;
    }
    refusals_load();

    for (i = 0; i < MAX_CLIENTS; i++)
        g_clients[i].fd = -1;

    if (GetVar("wasabi.key", g_key, sizeof(g_key), 0) <= 0)
        g_key[0] = '\0';

    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) {
        Printf("wasabid: no bsdsocket.library - is the TCP/IP stack up?\n");
        return RETURN_FAIL;
    }
    payload = AllocMem(MAX_PAYLOAD, MEMF_ANY);
    if (!payload) {
        CloseLibrary(SocketBase);
        Printf("wasabid: out of memory\n");
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
        ULONG sigs = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_F;
        int nfds = listen_fd;
        LONG n;

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
            BOOL busy = (g_run_client >= 0 || g_dbg_client >= 0 ||
                         g_snoop_client >= 0);
            tv.tv_secs  = busy ? 0 : 2;
            tv.tv_micro = busy ? 50000 : 0;
        }

        n = WaitSelect(nfds + 1, &rd, NULL, NULL, &tv, &sigs);

        if (sigs & SIGBREAKF_CTRL_C)
            break;

        if (g_run_client >= 0)
            if (!pump_run())
                drop(g_run_client);

        if (g_dbg_client >= 0)
            if (!debug_pump())
                drop(g_dbg_client);

        if (g_snoop_client >= 0)
            if (!snoop_pump())
                drop(g_snoop_client);

        refusals_save(FALSE);            /* rate-limited to once a minute */

        if (n <= 0)
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
                }
            }
        }

        for (i = 0; i < MAX_CLIENTS; i++) {
            UBYTE tag;
            LONG len;
            if (g_clients[i].fd < 0 || !FD_ISSET(g_clients[i].fd, &rd))
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
    refusals_save(TRUE);
    debug_stop();                        /* clears client and removes patch */
    snoop_stop();
    /* A task may still be between the snoop stub's use-count bump and its
     * rts; give stragglers a moment before this code segment goes away. */
    {
        int w;
        for (w = 0; wasabi_snoop_users && w < 50; w++)
            Delay(2);
    }
    if (g_dbg_patched || snoop_stuck())  /* could not: unloading now = Guru */
        Printf("wasabid: WARNING - a SetFunction patch could not be removed "
               "(someone patched over it). Do NOT let this binary unload.\n");
    for (i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].fd >= 0)
            CloseSocket(g_clients[i].fd);
    if (disco_fd >= 0) CloseSocket(disco_fd);
    if (listen_fd >= 0) CloseSocket(listen_fd);
    FreeMem(payload, MAX_PAYLOAD);
    CloseLibrary(SocketBase);

    /*
     * Relaunch now that the port is free. GetProgramName() gives the path
     * we were invoked by (C:wasabid, RAM:wasabid.b6, ...), so the fresh
     * daemon keeps our identity and port. Run detaches it; we then exit.
     */
    if (g_restart) {
        char self[128], cmd[160];
        if (GetProgramName(self, sizeof(self)) && self[0]) {
            sprintf(cmd, "Run >NIL: %s %d%s", self, g_port, g_extra_args);
            Execute(cmd, 0, 0);
        }
    }
    return RETURN_OK;
}
