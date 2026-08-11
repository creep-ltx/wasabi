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
#include <stdio.h>

#define VERSION_STR "wasabid 0.1"
/* 'used' so the optimizer cannot drop it - C:Version reads this string. */
static const char *verstag __attribute__((used)) =
    "$VER: wasabid 0.1 (12.8.2026)";

#define PROTO_VERSION   1
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

struct Client {
    int   fd;
    BOOL  hello;
};

static struct Client g_clients[MAX_CLIENTS];
static char g_key[128];
static BOOL g_quit;

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

static BOOL send_all(int fd, const UBYTE *buf, LONG len)
{
    while (len > 0) {
        LONG n = send(fd, (void *)buf, len, 0);
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
        LONG n = recv(fd, (void *)buf, len, 0);
        if (n <= 0)
            return FALSE;
        buf += n;
        len -= n;
    }
    return TRUE;
}

/*
 * Read one whole frame. Blocking: select() has already told us bytes are
 * waiting, and a client that sends half a frame then stalls will hold up
 * the daemon. Acceptable on a LAN with one user; the fix, if it ever
 * bites, is a per-client input buffer and a real state machine.
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

/* --- commands ------------------------------------------------------ */

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
        LONG n = sprintf(line, "%c %ld %ld %ld %ld %ld %s\n",
                         fib->fib_DirEntryType > 0 ? 'd' : 'f',
                         (long)fib->fib_Size,
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
 * Write to a sibling temp name and rename over the target at the end, so
 * an interrupted upload never leaves a half-written binary where a
 * working one used to be.
 */
static BOOL cmd_put(int fd, ULONG size, ULONG prot, const char *path)
{
    char tmp[300];
    BPTR fh;
    UBYTE *buf;
    ULONG got = 0;
    BOOL ok = TRUE;

    sprintf(tmp, "%.280s.wasabi-tmp", path);
    fh = Open(tmp, MODE_NEWFILE);
    if (!fh)
        return send_err(fd, "cannot create the temporary file");

    buf = AllocMem(MAX_PAYLOAD, MEMF_ANY);
    if (!buf) {
        Close(fh);
        DeleteFile(tmp);
        return send_err(fd, "out of memory");
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
            UBYTE w[128];
            LONG bl = (LONG)strlen(VERSION_STR);
            w[0] = 0; w[1] = PROTO_VERSION;
            w[2] = (UBYTE)(bl >> 8); w[3] = (UBYTE)bl;
            memcpy(w + 4, VERSION_STR, bl);
            return send_frame(fd, T_WELCOME, w, 4 + bl);
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

    case T_REBOOT:
        return cmd_reboot(fd, len >= 4 ? get_be32(p) : 0);

    case T_DEBUG:
    case T_SNOOP:
        /*
         * Phase 2. These need SetFunction() patches on RawPutChar and on
         * the dos.library entry points - see PROTOCOL.md. Saying so
         * plainly beats a stream that silently never emits anything.
         */
        return send_err(fd, "debug/snoop are not in this build yet");

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
    CloseSocket(g_clients[cl].fd);
    g_clients[cl].fd = -1;
    g_clients[cl].hello = FALSE;
}

int main(int argc, char **argv)
{
    int listen_fd = -1, disco_fd = -1, port = DEF_PORT, i;
    UBYTE *payload;

    (void)argc; (void)argv;

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

        /* Poll briskly while a command is producing output. */
        tv.tv_secs  = g_run_client >= 0 ? 0 : 2;
        tv.tv_micro = g_run_client >= 0 ? 50000 : 0;

        n = WaitSelect(nfds + 1, &rd, NULL, NULL, &tv, &sigs);

        if (sigs & SIGBREAKF_CTRL_C)
            break;

        if (g_run_client >= 0)
            if (!pump_run())
                drop(g_run_client);

        if (n <= 0)
            continue;

        if (disco_fd >= 0 && FD_ISSET(disco_fd, &rd))
            answer_probe(disco_fd, port);

        if (FD_ISSET(listen_fd, &rd)) {
            int fd = accept(listen_fd, NULL, NULL);
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
    for (i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].fd >= 0)
            CloseSocket(g_clients[i].fd);
    if (disco_fd >= 0) CloseSocket(disco_fd);
    if (listen_fd >= 0) CloseSocket(listen_fd);
    FreeMem(payload, MAX_PAYLOAD);
    CloseLibrary(SocketBase);
    return RETURN_OK;
}
