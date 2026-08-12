#!/usr/bin/env python3
"""mock-wasabid - a host-side stand-in for the Amiga daemon.

Serves the Wasabi protocol out of a host directory so the client can be
exercised end to end with no Amiga in the loop. It is also the reference
the C daemon is written against: where the two disagree about the wire,
this file is what the client was tested with.

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

ROOT = "/tmp/fakeamiga"
KEY = ""


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
            self.send(WELCOME, struct.pack(">H", VERSION) +
                      pack_str("mock-wasabid on %s" % os.uname().nodename))
            # Once a stream is subscribed, alternate between watching for
            # further frames (a second subscription - debug and snoop may
            # share one connection, as on the C daemon) and emitting.
            while True:
                if self.subs and not self.buf:
                    r, _, _ = select.select([self.request], [], [], 0.35)
                    if not r:
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
            self.send_data(
                ("mock-wasabid, protocol v%d\nroot: %s\nhost: %s\n"
                 % (VERSION, ROOT, os.uname().nodename)).encode("latin-1"))
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
                secs = int(st.st_mtime) - AMIGA_EPOCH
                days, rem = divmod(max(secs, 0), 86400)
                mins, s = divmod(rem, 60)
                lines.append("%s %d %d %d %d %d %s" % (
                    "d" if os.path.isdir(os.path.join(full, name)) else "f",
                    st.st_size, 0, days, mins, s * 50, name))
            self.send_data(("\n".join(lines) + "\n").encode("latin-1"))
        except (OSError, ValueError) as exc:
            self.err(str(exc), 205)

    def do_put(self, payload):
        size, _prot = struct.unpack_from(">II", payload, 0)
        path, _ = unpack_str(payload, 8)
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

    SNOOP_SAMPLES = [  # shaped like the C daemon's snoop_format() output
        ("cfile", 'Open("S:Startup-Sequence", read) = ok'),
        ("Shell", 'Lock("DH0:Tools", read) = ok'),
        ("cfile", 'LoadSeg("C:List") = ok'),
        ("wasabid", 'GetVar("wasabi.key") = fail (err 232)'),
    ]

    def do_stream(self, tag, payload):
        """Register a subscription; the handler loop does the emitting."""
        if tag == SNOOP:
            pattern, _ = unpack_str(payload, 4)
            self.subs[1] = {"rx": amiga_pattern(pattern) if pattern else None,
                            "i": 0}
        else:
            self.subs[0] = {"i": 0}

    def emit_streams(self):
        """One synthetic line per subscribed stream per idle tick."""
        if 0 in self.subs:
            st = self.subs[0]
            st["i"] += 1
            self.emit("exfat: ReadCacheNode(0x08cc3140, %d)\n" % st["i"])
        if 1 in self.subs:
            st = self.subs[1]
            for _ in range(len(self.SNOOP_SAMPLES)):
                task, line = self.SNOOP_SAMPLES[st["i"] % len(self.SNOOP_SAMPLES)]
                st["i"] += 1
                if st["rx"] and not st["rx"].match(task):
                    continue
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
    global ROOT, KEY
    p = argparse.ArgumentParser()
    p.add_argument("--root", default=ROOT)
    p.add_argument("--port", type=int, default=1234)
    p.add_argument("--key", default="")
    args = p.parse_args()
    ROOT, KEY = args.root, args.key
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
