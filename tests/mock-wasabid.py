#!/usr/bin/env python3
"""mock-wasabid - a host-side stand-in for the Amiga daemon.

Serves the Wasabi protocol out of a host directory so the client can be
exercised end to end with no Amiga in the loop. It is also the reference
the C daemon is written against: where the two disagree about the wire,
this file is what the client was tested with.

Known divergences from the C daemon, all because this is Unix underneath:

  - RUN's merge/detach flags are honoured here; the daemon ignores both
    (reserved wire protocol - see PROTOCOL.md).
  - stderr arrives separately here; OS 3.x has no SYS_Error, so the
    daemon merges everything onto STDOUT.
  - REBOOT and RESTART reply OK and do nothing.
  - Commands run concurrently here; the daemon serves one RUN at a time.

    ./mock-wasabid.py --root /tmp/fakeamiga --port 1234 --key hunter2
"""

import argparse
import os
import re
import select
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time

VERSION = 1
MAX_PAYLOAD = 65536
AMIGA_EPOCH = 252460800

HELLO, WELCOME, ERR, OK, PING, PONG = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
PUT, GET, DATA, END, LS, DEL, MKDIR = 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16
RUN, STDOUT, STDERR, EXIT = 0x20, 0x21, 0x22, 0x23
DEBUG, SNOOP, LOG = 0x30, 0x31, 0x32
REBOOT, INFO, RESTART, PS, KILL, SPEED = 0x40, 0x41, 0x42, 0x43, 0x44, 0x45
QUIT, INSTALL, GRAB, SCREEN = 0x46, 0x47, 0x48, 0x49

ROOT = "/tmp/fakeamiga"
KEY = ""
PORT = 1234
# What this instance calls itself in WELCOME. A trial instance spawned by
# `wasabi update` is standing in for a specific binary, so it is started
# with that binary's version - the real daemon's banner is its version.
BANNER = None
# What this mock claims to support, appended to WELCOME. --caps lets a
# test play an older daemon; --caps '' plays one from before the list.
CAPS = ("ping,info,ls,put,get,run,del,mkdir,debug,snoop,"
        "reboot,restart,ps,kill,speed,speedfile,quit,install,grab,screen,"
        "hb,guru,snoopentry")
# --drop-stream-after N: close the FIRST subscribed stream connection
# after N emit ticks, once per mock lifetime - the client's reconnect
# then finds a mock that behaves. This is how the suite proves the
# stream comes back from a mid-flight disconnect.
DROP_AFTER = 0
_DROPPED = [False]


def drop_once():
    if _DROPPED[0]:
        return False
    _DROPPED[0] = True
    return True
# Off-LAN connections the daemon has turned away, reported in WELCOME.
REFUSED = 0
# The path this mock pretends to be running from, so it can refuse a plain
# PUT over itself exactly as the daemon does.
SELF = "C:wasabid"
VER_TAG = b"$VER: wasabid "


def ver_of(path):
    """'wasabid 0.1b14' out of a file's $VER tag, or None."""
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError:
        return None
    i = blob.find(VER_TAG)
    if i < 0:
        return None
    end = blob.find(b"\0", i)
    tag = blob[i + 6:end if end > 0 else i + 120].decode("latin-1", "replace")
    return " ".join(tag.split()[:2])


def pack_str(s):
    b = s.encode("latin-1", "replace") if isinstance(s, str) else s
    return struct.pack(">H", len(b)) + b


def unpack_str(buf, off=0):
    (n,) = struct.unpack_from(">H", buf, off)
    off += 2
    return buf[off:off + n].decode("latin-1"), off + n


def amiga_pattern(pat):
    """AmigaDOS task filter -> anchored regex: '#?' and '*' are any-run,
    '?' any single char, case-insensitive, whole-name match - the same
    semantics as the C daemon's pat_match()."""
    out, i = [], 0
    while i < len(pat):
        if pat[i:i + 2] == "#?" or pat[i] == "*":
            out.append(".*")
            i += 2 if pat[i] == "#" else 1
        elif pat[i] == "?":
            out.append(".")
            i += 1
        else:
            out.append(re.escape(pat[i]))
            i += 1
    return re.compile("".join(out) + r"\Z", re.I)


def amiga_path(p):
    """Map 'DH0:tools/foo' or 'L:x' onto a path under ROOT, safely."""
    p = p.replace(":", "/", 1) if ":" in p else p
    p = p.replace("//", "/").lstrip("/")
    full = os.path.realpath(os.path.join(ROOT, p))
    root = os.path.realpath(ROOT)
    if full != root and not full.startswith(root + os.sep):
        raise ValueError("path escapes the fake root")
    return full


class Handler(socketserver.BaseRequestHandler):
    # --- framing ---
    def setup(self):
        self.buf = b""
        self.seqs = {0: 0, 1: 0}     # per stream, like the C daemon
        self.subs = {}               # stream subscriptions on this conn
        self.sendlock = threading.Lock()

    def send(self, tag, payload=b""):
        # The stdout and stderr pumps are separate threads; without this
        # lock two sendall()s interleave *inside* a frame and the client
        # sees garbage rather than merely out-of-order output.
        with self.sendlock:
            self.request.sendall(
                struct.pack(">BI", tag, len(payload)) + payload)

    def _fill(self, n):
        while len(self.buf) < n:
            chunk = self.request.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def recv(self):
        self._fill(5)
        tag, length = struct.unpack(">BI", self.buf[:5])
        if length > MAX_PAYLOAD:
            raise ValueError("oversized frame")
        self._fill(5 + length)
        payload = self.buf[5:5 + length]
        self.buf = self.buf[5 + length:]
        return tag, payload

    def err(self, msg, code=0):
        self.send(ERR, struct.pack(">I", code) + pack_str(msg))

    def send_data(self, blob):
        for i in range(0, len(blob), MAX_PAYLOAD):
            self.send(DATA, blob[i:i + MAX_PAYLOAD])
        self.send(END)

    # --- session ---
    def handle(self):
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            tag, payload = self.recv()
            if tag != HELLO:
                return self.err("expected HELLO")
            (ver,) = struct.unpack_from(">H", payload, 0)
            key, _ = unpack_str(payload, 2)
            if ver != VERSION:
                return self.err("protocol v%d not supported" % ver)
            if key != KEY:
                return self.err("bad key")
            welcome = struct.pack(">H", VERSION) + pack_str(
                BANNER or "mock-wasabid on %s" % os.uname().nodename)
            if CAPS:
                welcome += pack_str(CAPS)
                welcome += struct.pack(">I", REFUSED)
            self.send(WELCOME, welcome)
            # Once a stream is subscribed, alternate between watching for
            # further frames (a second subscription - debug and snoop may
            # share one connection, as on the C daemon) and emitting.
            ticks = 0
            while True:
                if self.subs and not self.buf:
                    r, _, _ = select.select([self.request], [], [], 0.35)
                    if not r:
                        ticks += 1
                        if DROP_AFTER and ticks >= DROP_AFTER \
                                and drop_once():
                            return       # hang up mid-stream, no goodbye
                        self.emit_streams()
                        continue
                tag, payload = self.recv()
                self.dispatch(tag, payload)
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass
        except Exception as exc:  # a mock may be loud about its own bugs
            try:
                self.err("mock daemon error: %s" % exc)
            except OSError:
                pass

    def dispatch(self, tag, payload):
        if tag == PING:
            self.send(PONG)
        elif tag == INFO:
            st = os.statvfs(ROOT)
            total = st.f_blocks * st.f_frsize // (1 << 20)
            free = st.f_bavail * st.f_frsize // (1 << 20)
            pct = 100 - (free * 100 // total) if total else 0
            self.send_data(
                ("mock-wasabid, protocol v%d\nroot: %s\nhost: %s\n"
                 "volumes:\n"
                 "  %-14s %6d MB total %6d MB free  %3d%% used\n"
                 % (VERSION, ROOT, os.uname().nodename,
                    "Mock:", total, free, pct)).encode("latin-1"))
        elif tag == LS:
            self.do_ls(payload)
        elif tag == PUT:
            self.do_put(payload)
        elif tag == GET:
            self.do_get(payload)
        elif tag == RUN:
            self.do_run(payload)
        elif tag == DEL:
            self.do_simple(payload, os.remove)
        elif tag == MKDIR:
            self.do_simple(payload, os.mkdir)
        elif tag == SPEED:
            self.do_speed(payload)
        elif tag == GRAB:
            self.do_grab()
        elif tag == SCREEN:
            self.do_screens(payload)
        elif tag == PS:
            self.do_ps()
        elif tag == KILL:
            self.do_kill(payload)
        elif tag in (DEBUG, SNOOP):
            self.do_stream(tag, payload)
        elif tag == REBOOT:
            self.send(OK)
            print("[mock] REBOOT requested (ignored)", file=sys.stderr)
        elif tag == RESTART:
            self.send(OK)
            print("[mock] RESTART requested (ignored)", file=sys.stderr)
        elif tag == QUIT:
            self.send(OK)
            print("[mock] QUIT - stopping", file=sys.stderr)
            sys.stderr.flush()
            os._exit(0)
        elif tag == INSTALL:
            self.do_install(payload)
        else:
            self.err("unknown tag 0x%02x" % tag)

    # --- commands ---
    def do_ls(self, payload):
        path, _ = unpack_str(payload)
        try:
            full = amiga_path(path)
            lines = []
            for name in os.listdir(full):
                st = os.stat(os.path.join(full, name))
                # A real DateStamp is local wall time, so build one the way
                # the Amiga would: shift the UTC epoch by this host's offset.
                # The client renders it verbatim.
                local = int(st.st_mtime) + time.localtime(st.st_mtime).tm_gmtoff
                secs = local - AMIGA_EPOCH
                days, rem = divmod(max(secs, 0), 86400)
                mins, s = divmod(rem, 60)
                lines.append("%s %d %d %d %d %d %s" % (
                    "d" if os.path.isdir(os.path.join(full, name)) else "f",
                    st.st_size, 0, days, mins, s * 50, name))
            self.send_data(("\n".join(lines) + "\n").encode("latin-1"))
        except (OSError, ValueError) as exc:
            self.err(str(exc), 205)

    @staticmethod
    def safe_path(p):
        try:
            return amiga_path(p)
        except ValueError:
            return "/nonexistent"

    def do_install(self, payload):
        """Rename a verified sidecar over the binary we run from."""
        sidecar, _ = unpack_str(payload)
        try:
            src = amiga_path(sidecar)
            dst = amiga_path(SELF)
            if not os.path.exists(src):
                return self.err("there is no such file to install", 205)
            if os.path.exists(dst):
                os.replace(dst, dst + ".bak")
            os.replace(src, dst)
            self.send(OK)
        except (OSError, ValueError) as exc:
            self.err(str(exc), 213)

    def do_put(self, payload):
        size, _prot = struct.unpack_from(">II", payload, 0)
        path, _ = unpack_str(payload, 8)
        # Refuse to overwrite the running daemon, as the C side does -
        # but drain the body first, or the DATA frames read back as
        # commands and desync the session.
        refuse = False
        try:
            refuse = os.path.exists(amiga_path(SELF)) and \
                     amiga_path(path) == amiga_path(SELF)
        except ValueError:
            pass
        if refuse:
            while True:
                tag, _data = self.recv()
                if tag != DATA:
                    break
            return self.err("that is the running daemon - use 'wasabi update'")
        got = b""
        while True:
            tag, data = self.recv()
            if tag == DATA:
                got += data
            elif tag == END:
                break
            else:
                return self.err("unexpected tag during PUT")
        if len(got) != size:
            return self.err("size mismatch: declared %d, got %d"
                            % (size, len(got)))
        try:
            full = amiga_path(path)
            tmp = full + ".wasabi-tmp"
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(tmp, "wb") as fh:
                fh.write(got)
            os.replace(tmp, full)
            self.send(OK)
        except (OSError, ValueError) as exc:
            self.err(str(exc), 213)

    def do_get(self, payload):
        path, _ = unpack_str(payload)
        try:
            with open(amiga_path(path), "rb") as fh:
                self.send_data(fh.read())
        except (OSError, ValueError) as exc:
            self.err(str(exc), 205)

    def do_run(self, payload):
        (flags,) = struct.unpack_from(">I", payload, 0)
        command, _ = unpack_str(payload, 4)
        merge = bool(flags & 1)
        detach = bool(flags & 2)
        # The three shapes `wasabi update` runs, standing in for AmigaDOS
        # and for the uploaded binary itself.
        word = command.split()
        if len(word) >= 2 and word[0] == "Version" and word[-1] == "FULL":
            ver = ver_of(self.safe_path(word[1].strip('"')))
            if not ver:
                self.send(STDOUT, b"object not found\n")
                return self.send(EXIT, struct.pack(">II", 20, 205))
            self.send(STDOUT, ("%s (2026-08-12)\n" % ver).encode("latin-1"))
            return self.send(EXIT, struct.pack(">II", 0, 0))

        if len(word) >= 2 and word[1] == "--selftest":
            ver = ver_of(self.safe_path(word[0]))
            nonce = word[2] if len(word) > 2 else "-"
            if not ver:
                self.send(STDOUT, b"selftest: FAILED\n")
                return self.send(EXIT, struct.pack(">II", 10, 0))
            self.send(STDOUT, ("wasabid-selftest-ok %s %s\n"
                               % (nonce, ver)).encode("latin-1"))
            return self.send(EXIT, struct.pack(">II", 0, 0))

        if word[:2] == ["Run", ">NIL:"] and len(word) >= 4:
            # A trial daemon on a spare port: spawn a real second mock,
            # so the client's live probe talks to something that answers.
            binary, altport = self.safe_path(word[2]), word[3]
            ver = ver_of(binary)
            # A binary carrying BREAK_SERVE plays a daemon that passes its
            # own self-test and then cannot serve: nothing is spawned, so
            # the client's live probe finds nobody on the port.
            try:
                with open(binary, "rb") as fh:
                    dead = b"BREAK_SERVE" in fh.read()
            except OSError:
                dead = True
            if ver and not dead:
                subprocess.Popen(
                    [sys.executable, os.path.abspath(__file__),
                     "--root", ROOT, "--port", altport, "--key", KEY,
                     "--banner", ver],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return self.send(EXIT, struct.pack(">II", 0, 0))
        try:
            proc = subprocess.Popen(
                command, shell=True, cwd=ROOT, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT if merge else subprocess.PIPE)
        except OSError as exc:
            return self.err(str(exc), 121)
        if detach:
            return self.send(EXIT, struct.pack(">II", 0, 0))

        def pump(stream, tag):
            for line in iter(stream.readline, b""):
                self.send(tag, line)

        threads = [threading.Thread(target=pump, args=(proc.stdout, STDOUT))]
        if not merge:
            threads.append(threading.Thread(
                target=pump, args=(proc.stderr, STDERR)))
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        rc = proc.wait()
        self.send(EXIT, struct.pack(">II", rc if rc >= 0 else 20, 0))

    def do_speed(self, payload):
        """Storage-free throughput: count-and-discard, or generate."""
        flags, size = struct.unpack_from(">II", payload, 0)
        if not size or size > 256 << 20:
            return self.err("size must be 1 byte to 256 MB")
        if flags & 1:
            self.send_data(b"\xa5" * size)
        else:
            got = 0
            while True:
                tag, data = self.recv()
                if tag == END:
                    break
                if tag != DATA:
                    return self.err("unexpected tag during SPEED")
                got += len(data)
            if got != size:
                return self.err("size mismatch")
            self.send(OK)

    def do_simple(self, payload, fn):
        path, _ = unpack_str(payload)
        try:
            fn(amiga_path(path))
            self.send(OK)
        except (OSError, ValueError) as exc:
            self.err(str(exc), 205)

    MOCK_TASKS = [  # addr, kind, pri, state, stack, cli, name, cmd - the
                    # daemon's ps wire format; con_handler is deliberately
                    # duplicated so the ambiguous-kill path is testable
        ("0x0804f010", "p", 0, "run", 4096, 1, "wasabid", "c:wasabid"),
        ("0x08051200", "p", 0, "wait", 4096, 2, "Background CLI", "Wait"),
        ("0x08032400", "t", 5, "wait", 6144, -1, "input.device", ""),
        ("0x08036000", "t", 5, "wait", 4096, -1, "con_handler", ""),
        ("0x08037000", "t", 5, "wait", 4096, -1, "con_handler", ""),
    ]

    def do_screens(self, payload):
        """Two screens, front first - enough to exercise the client.

        Same reply shape as the daemon: the listing always goes out as
        DATA, then either END or - for a --to-front title nothing
        matches - an ERR in END's place."""
        (_flags,) = struct.unpack_from(">I", payload, 0)
        want, _ = unpack_str(payload, 4)
        titles = ["CygnusEd Professional V4.2", "Workbench Screen"]
        rows = ["0x00001111 640 256 2 %s" % titles[0],
                "0x00002222 1280 960 24 %s" % titles[1]]
        blob = ("\n".join(rows) + "\n").encode("latin-1")
        for i in range(0, len(blob), MAX_PAYLOAD):
            self.send(DATA, blob[i:i + MAX_PAYLOAD])
        if want and want.lower() not in (t.lower() for t in titles):
            return self.err("no screen with that title")
        self.send(END)

    def do_grab(self):
        """A tiny synthetic screen: header then raw RGB rows."""
        w, h = 8, 4
        px = bytearray()
        for y in range(h):
            for x in range(w):
                px += bytes(((x * 32) % 256, (y * 64) % 256, 128))
        self.send_data(struct.pack(">III", w, h, 3) + bytes(px))

    def do_ps(self):
        lines = ["%s %s %d %s %d %d %s\t%s" % r for r in self.MOCK_TASKS]
        self.send_data(("\n".join(lines) + "\n").encode("latin-1"))

    def do_kill(self, payload):
        target, _ = unpack_str(payload, 4)
        tl = target.lower()
        hits = [r for r in self.MOCK_TASKS
                if r[0].lower() == tl or r[6].lower() == tl or
                (r[7] and r[7].lower() == tl)]
        if not hits:
            return self.err("no task or process by that name", 205)
        if len(hits) > 1:
            return self.err(
                "ambiguous - several tasks match; use the 0x address from ps")
        if hits[0][6] == "wasabid":
            return self.err("that is wasabid itself - use restart or reboot")
        self.send(OK)

    # Shaped like the C daemon's snoop_format() output. ORDER MATTERS:
    # one sample goes out per idle tick, so a line's position is how
    # many ticks a test must wait to see it. The tested lines are
    # front-loaded deliberately - burying them cost a macOS CI run,
    # because the runner is slower than the machine they were timed on.
    SNOOP_SAMPLES = [
        ("cfile", 'Open("S:Startup-Sequence", read) = ok'),
        ("wasabid", 'GetVar("wasabi.key") = fail (err 232)'),
        # the poll noise bsdsocket really makes, three ticks of it, so
        # folding and --output minimal have something to work on
        ("wasabid", 'OpenLibrary("dos.library", v0) = ok'),
        ("wasabid", 'OpenLibrary("dos.library", v0) = ok'),
        ("wasabid", 'OpenLibrary("dos.library", v0) = ok'),
        ("c:wasabid", 'Open("T:wasabi-run-5", readwrite) = ok'),
        ("wasabi-runner", 'Lock("C:List", read) = ok'),
        ("Shell", 'Open("T:wasabi-run-5", readwrite) = ok'),
        ("Shell", 'Lock("DH0:Tools", read) = ok'),
        ("cfile", 'LoadSeg("C:List") = ok'),
    ]

    def do_stream(self, tag, payload):
        """Register a subscription; the handler loop does the emitting."""
        if tag == SNOOP:
            (flags,) = struct.unpack_from(">I", payload, 0)
            pattern, _ = unpack_str(payload, 4)
            self.subs[1] = {"rx": amiga_pattern(pattern) if pattern else None,
                            "i": 0, "entry": bool(flags & 1)}
        else:
            self.subs[0] = {"i": 0}

    def emit_streams(self):
        """One synthetic line per subscribed stream per idle tick, plus
        the empty heartbeat LOG the real daemon sends - it must render
        as nothing at all on the client."""
        for stream in self.subs:
            self.emit("", stream=stream)
        if 0 in self.subs:
            st = self.subs[0]
            st["i"] += 1
            self.emit("exfat: ReadCacheNode(0x08cc3140, %d)\n" % st["i"])
            if st["i"] == 2:
                self.emit("[wasabi: DEADEND ALERT #80000004 in task "
                          "'SysInfo' (0x082a1340)]\n")
        if 1 in self.subs:
            st = self.subs[1]
            for _ in range(len(self.SNOOP_SAMPLES)):
                task, line = self.SNOOP_SAMPLES[st["i"] % len(self.SNOOP_SAMPLES)]
                st["i"] += 1
                if st["rx"] and not st["rx"].match(task):
                    continue
                if st.get("entry"):
                    self.emit("%-20s %s ...\n"
                              % (task, line.split(" = ")[0]), stream=1)
                self.emit("%-20s %s\n" % (task, line), stream=1)
                break

    def emit(self, text, stream=0):
        self.seqs[stream] += 1
        self.send(LOG,
                  struct.pack(">II", stream, self.seqs[stream]) + pack_str(text))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def discovery_responder(port, name):
    """Answer WASABI?1 broadcasts so the client can find us."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", port))
    reply = ("WASABI!1 %s %d mock-wasabid (not a real Amiga)\n"
             % (name, port)).encode("latin-1")
    while True:
        try:
            data, addr = sock.recvfrom(1024)
        except OSError:
            return
        if data.strip() == b"WASABI?1":
            sock.sendto(reply, addr)


def main():
    global ROOT, KEY, PORT, BANNER, CAPS, REFUSED, DROP_AFTER
    p = argparse.ArgumentParser()
    p.add_argument("--root", default=ROOT)
    p.add_argument("--port", type=int, default=1234)
    p.add_argument("--key", default="")
    p.add_argument("--banner", default=None,
                   help="what to call ourselves in WELCOME")
    p.add_argument("--caps", default=CAPS,
                   help="capability list; empty plays a pre-caps daemon")
    p.add_argument("--refused", type=int, default=0,
                   help="off-LAN refusals to report in WELCOME")
    p.add_argument("--drop-stream-after", type=int, default=0, metavar="N",
                   help="close the first stream connection after N emit "
                        "ticks (once) - for the reconnect tests")
    args = p.parse_args()
    REFUSED = args.refused
    DROP_AFTER = args.drop_stream_after
    ROOT, KEY, PORT, BANNER = args.root, args.key, args.port, args.banner
    CAPS = args.caps
    os.makedirs(ROOT, exist_ok=True)
    threading.Thread(
        target=discovery_responder,
        args=(args.port, os.uname().nodename), daemon=True).start()
    srv = Server(("127.0.0.1", args.port), Handler)
    print("mock-wasabid on 127.0.0.1:%d serving %s" % (args.port, ROOT),
          file=sys.stderr)
    srv.serve_forever()


if __name__ == "__main__":
    main()
