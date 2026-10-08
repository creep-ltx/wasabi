"""wasabi view - the Amiga's screen in a window, with its mouse and keyboard.

The bridge between a web page (view/dist, built from view/src) and
wasabid. It serves the page, and for each page that connects it holds
one connection to the Amiga and runs one worker thread over it:

    input first: every pointer move, button and key the page sent,
                 as wasabid MOUSE and KEY commands (~3 ms each);
    then a grab: the front screen, compared row by row with the last
                 one, and only the changed rows sent on.

The daemon serves one command at a time, so doing both from one thread
in that order is what keeps the pointer responsive: input never queues
behind more than one grab (~80-130 ms on the A1200's 1280x960).

Standard library only, like the rest of the client - the WebSocket is
done by hand (RFC 6455, unfragmented messages, which is what browsers
send). Imported by `wasabi view`; the wasabi module is passed in as `w`.
"""

import base64
import hashlib
import json
import os
import queue
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.realpath(__file__))
DIST = os.path.join(HERE, "view", "dist")
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# Qualifier bit of each Amiga modifier's raw key, so a key pressed with
# Right Amiga held carries the bit Intuition's shortcuts look for.
MOD_BITS = {0x60: 0x01, 0x61: 0x02, 0x62: 0x04, 0x63: 0x08,
            0x64: 0x10, 0x65: 0x20, 0x66: 0x40, 0x67: 0x80}
QUAL_REPEAT = 0x0200
NOPOS = -32768
MIN_FRAME = 0.1                 # GRAB fallback: at most ~10 a second
# LIVE: a frame costs the Amiga ~20 ms of work (1280x960 RTG, measured)
# whether or not anything changed, so look often only while something
# is happening - input from the page, or a picture that changed.
LIVE_BUSY = 0.05                # 20 a second while busy
LIVE_IDLE = 0.2                 # 5 a second once all is still
LIVE_BUSY_FOR = 2.0             # "busy" lasts this long after activity
DEBUG = bool(os.environ.get("WASABI_VIEW_DEBUG"))   # log input to stderr
NO_LIVE = bool(os.environ.get("WASABI_VIEW_NOLIVE"))  # force GRAB, to compare


def settings_path():
    return os.path.join(os.environ.get("XDG_CONFIG_HOME",
                                       os.path.expanduser("~/.config")),
                        "wasabi", "view.json")


# --- WebSocket, by hand ------------------------------------------------


class WebSocket:
    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()     # the worker and the reader share it
        self.closed = False

    MAX_FRAME = 1024 * 1024             # the page sends small JSON only

    def _read(self, n, idle_ok=False):
        buf = b""
        while len(buf) < n:
            try:
                chunk = self.sock.recv(n - len(buf))
            except socket.timeout:
                if idle_ok and not buf:
                    self._send(9, b"")  # ping: is the page still there?
                    if self.closed:
                        raise ConnectionError("page gone")
                    continue
                raise ConnectionError("page stalled")
            if not chunk:
                raise ConnectionError("page closed")
            buf += chunk
        return buf

    def recv(self):
        """The next text message (str) or None when the page closes."""
        while True:
            b0, b1 = self._read(2, idle_ok=True)
            op, masked, n = b0 & 0x0F, b1 & 0x80, b1 & 0x7F
            if n == 126:
                (n,) = struct.unpack(">H", self._read(2))
            elif n == 127:
                (n,) = struct.unpack(">Q", self._read(8))
            if n > self.MAX_FRAME:
                raise ConnectionError("frame too big")
            mask = self._read(4) if masked else b"\0\0\0\0"
            data = bytearray(self._read(n))
            for i in range(n):
                data[i] ^= mask[i & 3]
            if op == 8:
                return None
            if op == 9:
                self._send(10, bytes(data))
                continue
            if op == 10:                # the page's answer to our ping
                continue
            if op == 1:
                return data.decode("utf-8", "replace")
            # binary or continuation frames: the page sends neither

    def _send(self, op, payload):
        n = len(payload)
        if n < 126:
            head = struct.pack(">BB", 0x80 | op, n)
        elif n < 65536:
            head = struct.pack(">BBH", 0x80 | op, 126, n)
        else:
            head = struct.pack(">BBQ", 0x80 | op, 127, n)
        with self.lock:
            if self.closed:
                return
            try:
                self.sock.sendall(head + payload)
            except OSError:             # timeouts included: a page that
                self.closed = True      # stopped reading is dropped

    def send_text(self, obj):
        self._send(1, json.dumps(obj).encode())

    def send_binary(self, data):
        self._send(2, data)


# --- one page's session with the Amiga ---------------------------------


class Session:
    def __init__(self, w, target, key, ws):
        self.w, self.target, self.key, self.ws = w, target, key, ws
        self.inbox = queue.Queue()
        self.early = []                  # taken off the queue while waiting
        self.alive = True
        self.qual = 0                    # held Amiga modifier bits
        self.prev = None                 # (width, height, pixels) last sent
        self.live = False                # the daemon has LIVE
        self.need_full = True            # the page needs every pixel
        self.active = 0.0                # when input or a change last came

    def stop(self):
        self.alive = False
        self.inbox.put(None)

    def run(self):
        conn = None
        while self.alive:
            if conn is None:
                try:
                    conn = self.w.Conn(self.target[0], self.target[1],
                                       self.key)
                    self.prev = None     # a fresh page needs every row
                    self.need_full = True
                    self.live = "live" in conn.caps and not NO_LIVE
                    self.ws.send_text({"t": "status", "connected": True,
                                       "banner": conn.banner,
                                       "live": self.live})
                except (OSError, self.w.WasabiError) as exc:
                    self.ws.send_text({"t": "status", "connected": False,
                                       "error": "cannot reach the Amiga: "
                                                "%s" % exc})
                    self._sleep(2.0)
                    continue
            try:
                self._cycle(conn)
            except (OSError, self.w.WasabiError) as exc:
                self.ws.send_text({"t": "status", "connected": False,
                                   "error": "lost the Amiga: %s" % exc})
                conn.close()
                conn = None
                self._sleep(1.0)
        if conn:
            self._release(conn)
            conn.close()

    def _sleep(self, secs):
        end = time.time() + secs
        while self.alive and time.time() < end:
            time.sleep(0.1)

    def _cycle(self, conn):
        t0 = time.time()
        if self._drain_input(conn):
            self.active = t0
        if self.live:
            if self._live(conn):
                self.active = time.time()
            busy = time.time() - self.active < LIVE_BUSY_FOR
            frame = LIVE_BUSY if busy else LIVE_IDLE
        else:
            self._grab(conn)
            frame = MIN_FRAME
        # Wait out the rest of the frame - but wake for input at once.
        # What woke us is kept aside, in order, for the next drain: put
        # back on the queue it would go behind newer input, and a key
        # could reach the Amiga before the Right Amiga pressed with it
        # (seen on the A1200: Right Amiga+E arrived as E, then Amiga).
        left = frame - (time.time() - t0)
        if left > 0:
            try:
                msg = self.inbox.get(timeout=left)
                if msg is not None:
                    self.early.append(msg)
            except queue.Empty:
                pass

    def _drain_input(self, conn):
        msgs, self.early = self.early, []
        while True:
            try:
                m = self.inbox.get_nowait()
            except queue.Empty:
                break
            if m is not None:
                msgs.append(m)
        busy = bool(msgs)
        # Only the last of a run of moves matters: the pointer goes
        # where it ended up, not through every point it passed.
        for i, m in enumerate(msgs):
            if m.get("t") == "move" and i + 1 < len(msgs) and \
                    msgs[i + 1].get("t") == "move":
                continue
            try:
                self._input(conn, m)
            except (KeyError, ValueError, TypeError, struct.error):
                # one malformed message is skipped: it used to end the
                # worker, freezing the picture and leaving any held
                # Right Amiga down on the Amiga (audit 4)
                continue
        return busy

    def _input(self, conn, m):
        w = self.w
        if DEBUG:
            print("[view] %.3f %r qual=0x%x" % (time.time() % 100, m, self.qual),
                  file=sys.stderr)
        t = m.get("t")
        if t == "move":
            conn.send(w.MOUSE, struct.pack(">HHHhh", 0, 0, 0,
                                           int(m["x"]), int(m["y"])))
            conn.expect_ok()
        elif t == "button":
            b = int(m["b"])
            if b not in (0, 1, 2):
                return
            conn.send(w.MOUSE, struct.pack(">HHHhh", 2 if m["down"] else 3,
                                           b, 0, NOPOS, NOPOS))
            conn.expect_ok()
        elif t == "text":
            self._text(conn, m)
        elif t == "press":
            # one key with modifiers, from the phone's key bar: modifier
            # keys down, the key, modifiers up - all in one command
            code = int(m["code"]) & 0x7F
            qual = int(m.get("qual", 0)) & 0xFF
            ev = []
            q = 0
            for mod, bit in sorted(MOD_BITS.items()):
                if qual & bit and mod != 0x62:
                    q |= bit
                    ev.append((mod, q))
            ev += [(code, q), (code | 0x80, q)]
            for mod, bit in sorted(MOD_BITS.items(), reverse=True):
                if q & bit:
                    q &= ~bit
                    ev.append((mod | 0x80, q))
            conn.send(w.KEY, struct.pack(">H", 0) +
                      b"".join(struct.pack(">HH", c, qq) for c, qq in ev))
            conn.expect_ok()
        elif t == "key":
            code = int(m["code"]) & 0x7F
            bit = MOD_BITS.get(code)
            if bit is not None:
                # Modifiers go down and up as they happen: a held Shift
                # or Right Amiga must stay held. They never auto-repeat.
                # Their own event carries their bit going down and has
                # lost it coming up, as the real keyboard reports it.
                if m["down"]:
                    self.qual |= bit
                else:
                    self.qual &= ~bit
                raw = code if m["down"] else code | 0x80
                conn.send(w.KEY, struct.pack(">HHH", 0, raw, self.qual))
                conn.expect_ok()
                return
            # Every other key goes as a whole press - down and up in one
            # command - the moment the PC key goes down, and again for
            # each of the PC's own repeats. input.device auto-repeats a
            # written key until its up arrives (measured on the A1200:
            # an up 0.8 s late typed "eeeeeee", 0.15 s late did not), so
            # a separately sent up made the Amiga's typing depend on the
            # bridge's timing. A late up did happen here (the ordering
            # bug fixed in _cycle); whole presses rule out the rest.
            if not m["down"]:
                return
            qual = self.qual | (QUAL_REPEAT if m.get("repeat") else 0)
            conn.send(w.KEY, struct.pack(">HHHHH", 0, code, qual,
                                         code | 0x80, self.qual))
            conn.expect_ok()

    def _text(self, conn, m):
        """Typed text from a phone's keyboard: the Amiga's keymap picks
        the keys (KEY text mode), so å and @ come out right."""
        data = str(m.get("s", ""))[:2048].replace("\n", "\r")
        try:
            raw = data.encode("latin-1")
        except UnicodeEncodeError:
            raw = data.encode("latin-1", "replace")
        if raw:
            conn.send(self.w.KEY, struct.pack(">H", 1) + raw)
            conn.expect_ok()

    def _release(self, conn):
        """Let go of every modifier and button: a page that vanished
        must not leave Right Amiga held down on the Amiga."""
        try:
            for code, bit in MOD_BITS.items():
                if self.qual & bit and code != 0x62:
                    self.qual &= ~bit
                    conn.send(self.w.KEY, struct.pack(">HHH", 0, code | 0x80,
                                                      self.qual))
                    conn.expect_ok()
            for b in (0, 1, 2):
                conn.send(self.w.MOUSE, struct.pack(">HHHhh", 3, b, 0,
                                                    NOPOS, NOPOS))
                conn.expect_ok()
        except (OSError, self.w.WasabiError):
            pass

    def _live(self, conn):
        """One LIVE frame: the daemon's messages go to the page as they
        are (they are already the page's format). TRUE if any pixel or
        the palette changed."""
        conn.send(self.w.LIVE, struct.pack(">I", 1 if self.need_full else 0))
        self.need_full = False
        changed = False
        while True:
            tag, payload = conn.recv()
            if tag == self.w.DATA:
                if payload[:1] in (b"\x11", b"\x12"):
                    changed = True
                self.ws.send_binary(payload)
            elif tag == self.w.END:
                break
            elif tag == self.w.ERR:
                self.need_full = True
                raise self.w.WasabiError(conn._errtext(payload))
            else:
                raise self.w.WasabiError("unexpected tag 0x%02x in LIVE" % tag)
        self.ws.send_text({"t": "frame", "changed": changed})
        return changed

    def _grab(self, conn):
        width, _, height, pixels, _, _ = self.w.grab_pixels(conn)
        prev = self.prev
        rowbytes = width * 3
        if prev is None or prev[0] != width or prev[1] != height:
            bands = [(0, height)]
        else:
            old = prev[2]
            dirty = [y for y in range(height)
                     if pixels[y * rowbytes:(y + 1) * rowbytes] !=
                     old[y * rowbytes:(y + 1) * rowbytes]]
            bands = []
            for y in dirty:
                # rows a few apart go as one band: fewer messages
                if bands and y - bands[-1][1] <= 8:
                    bands[-1] = (bands[-1][0], y + 1)
                else:
                    bands.append((y, y + 1))
            bands = [(a, b - a) for a, b in bands]
        for y, rows in bands:
            self.ws.send_binary(
                struct.pack(">BHHHH", 1, width, height, y, rows) +
                pixels[y * rowbytes:(y + rows) * rowbytes])
        self.prev = (width, height, pixels)
        self.ws.send_text({"t": "frame", "bands": len(bands)})


# --- the HTTP side -----------------------------------------------------


TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript",
         ".css": "text/css", ".svg": "image/svg+xml", ".png": "image/png",
         ".webmanifest": "application/manifest+json",
         ".woff2": "font/woff2", ".woff": "font/woff", ".ttf": "font/ttf"}


def make_handler(w, fleet, server_state, port, listen, auth=None):
    import http.cookies
    import urllib.parse
    from wasabi_api import ApiError, Auth
    hosts = {"127.0.0.1:%d" % port, "localhost:%d" % port,
             "%s:%d" % (listen, port)}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):      # quiet: this is an app window
            pass

        def length(self, limit):
            """The request's declared body size, or None (after answering
            400/413) when it is missing-but-needed, not a number, below 0
            or over `limit`. A negative size made rfile.read() read until
            the peer hung up, and a huge one was allocated up front - both
            before any login (audit 4)."""
            raw = self.headers.get("Content-Length", "0") or "0"
            try:
                n = int(raw)
            except ValueError:
                n = -1
            if n < 0:
                self.json(400, {"error": "bad Content-Length"})
                return None
            if n > limit:
                self.json(413, {"error": "that is too big"})
                return None
            return n

        def body_json(self):
            """A small JSON object body, or None after answering 400."""
            n = self.length(64 * 1024)
            if n is None:
                return None
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                body = None
            if not isinstance(body, dict):
                self.json(400, {"error": "bad JSON"})
                return None
            return body

        def session(self):
            """The login cookie, read by hand: SimpleCookie gives up at
            the first cookie it cannot parse, and another app on the
            same host (DSM, Portainer - browsers share cookies across
            ports) setting one made logging in impossible (audit 4)."""
            for part in self.headers.get("Cookie", "").split(";"):
                k, _, v = part.strip().partition("=")
                if k == Auth.COOKIE:
                    return v.strip()
            return ""

        def logged_in(self):
            return auth is None or auth.check(self.session())

        def set_session(self, token, body=b'{"ok": true}'):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Set-Cookie", "%s=%s; HttpOnly; SameSite=Strict; "
                             "Path=/; Max-Age=%d" % (Auth.COOKIE, token,
                                                     Auth.DAYS * 86400))
            self.end_headers()
            self.wfile.write(body)

        def auth_call(self, method, path):
            """The login: open to all - it is how one gets in."""
            if method == "GET":
                return self.json(200, {
                    "required": auth is not None,
                    "setup": bool(auth and auth.needs_setup()),
                    "logged_in": self.logged_in(),
                    "version": fleet.first.api.version})
            body = self.body_json()
            if body is None:
                return
            if auth is None:
                return self.json(404, {"error": "no login on this server"})
            try:
                if path == "/api/auth/setup":
                    return self.set_session(auth.setup(str(body.get("password", ""))))
                if path == "/api/auth/login":
                    return self.set_session(auth.login(str(body.get("password", ""))))
                if path == "/api/auth/logout":
                    auth.logout(self.session())
                    return self.set_session("", b'{"ok": true}')
            except ApiError as exc:
                return self.json(exc.code, {"error": str(exc)})
            return self.json(404, {"error": "no such call"})

        def trusted(self, changes):
            """See wasabi_api's docstring: Host names this server, and a
            change carries X-Wasabi, which another site cannot send. A
            server with a login is reached by any of its names (the NAS
            has three), and the login cookie does the Host check's job."""
            if auth is None and self.headers.get("Host", "") not in hosts:
                self.reply(403, "text/plain", b"wrong host")
                return False
            if changes and self.headers.get("X-Wasabi") != "1":
                self.reply(403, "text/plain", b"missing X-Wasabi")
                return False
            return True

        def api(self, method):
            url = urllib.parse.urlsplit(self.path)
            body = {}
            if method == "POST":
                body = self.body_json()
                if body is None:
                    return
            query = urllib.parse.parse_qs(url.query)
            if url.path in ("/api/machines", "/api/machines/discover"):
                try:
                    if url.path == "/api/machines/discover":
                        return self.json(200, fleet.discover())
                    if method == "POST":
                        return self.json(200, fleet.change(body))
                    return self.json(200, fleet.listing())
                except ApiError as exc:
                    return self.json(exc.code, {"error": str(exc)})
                except (ValueError, TypeError, AttributeError,
                        OverflowError) as exc:
                    return self.json(400, {"error": "bad request (%s)" % exc})
            api = self.machine().api
            try:
                out = api.dispatch(method, url.path, query, body)
            except ApiError as exc:
                return self.json(exc.code, {"error": str(exc)})
            except OSError as exc:
                return self.json(500, {"error": str(exc)})
            self.json(200, out)

        def upload(self):
            """A file from the page, as the raw request body (drag and
            drop, the phone's file picker): ?side=amiga|local&dir=&name="""
            q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            g = lambda k: q.get(k, [""])[0]           # noqa: E731
            api = self.machine().api
            n = self.length(api.UPLOAD_MAX)
            if n is None:
                return
            data = self.rfile.read(n)
            try:
                out = api.upload(g("side"), g("dir"), g("name"), data,
                                 g("force") == "1")
            except ApiError as exc:
                return self.json(exc.code, {"error": str(exc)})
            except (ValueError, TypeError, AttributeError, OverflowError,
                    KeyError) as exc:
                # a wrong-typed value from the page: an answer, not a
                # dropped connection (audit 4)
                return self.json(400, {"error": "bad request (%s)" % exc})
            except OSError as exc:
                return self.json(500, {"error": str(exc)})
            self.json(200, out)

        def json(self, code, obj):
            self.reply(code, "application/json", json.dumps(obj).encode())

        def machine(self):
            """The machine this request is for: ?m=<id>, else the first."""
            q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            return fleet.get(q.get("m", [""])[0])

        def do_POST(self):
            if not self.trusted(changes=True):
                return
            if self.path.startswith("/api/auth/"):
                return self.auth_call("POST", self.path.split("?", 1)[0])
            if not self.logged_in():
                return self.json(401, {"error": "please log in"})
            if self.path.split("?", 1)[0] == "/api/upload":
                return self.upload()
            if self.path.startswith("/api/"):
                return self.api("POST")
            self.reply(404, "text/plain", b"not found")

        def do_GET(self):
            if not self.trusted(changes=False):
                return
            path = self.path.split("?", 1)[0]
            if path.startswith("/api/auth/"):
                return self.auth_call("GET", path)
            guarded = path.startswith("/ws") or path.startswith("/shots/") or \
                path.startswith("/api/")
            if guarded and not self.logged_in():
                return self.json(401, {"error": "please log in"})
            if path in ("/ws", "/ws/logs", "/ws/run", "/ws/hello"):
                origin = self.headers.get("Origin", "")
                allowed = hosts | {self.headers.get("Host", "")} if auth \
                    else hosts
                if origin and origin.split("://", 1)[-1] not in allowed:
                    return self.reply(403, "text/plain", b"wrong origin")
                if path == "/ws/hello":
                    return self.websocket_hello()
                if path == "/ws/logs":
                    return self.websocket_logs()
                if path == "/ws/run":
                    return self.websocket_run()
                return self.websocket()
            if path.startswith("/shots/"):
                try:
                    p = fleet.first.api.shot_path(urllib.parse.unquote(path[7:]))
                except ApiError:
                    return self.reply(404, "text/plain", b"not found")
                with open(p, "rb") as fh:
                    return self.reply(200, "image/png", fh.read())
            if path.startswith("/api/") and path != "/api/settings":
                return self.api("GET")
            if path == "/api/settings":
                try:
                    with open(settings_path()) as fh:
                        body = fh.read().encode()
                except OSError:
                    body = b"{}"
                return self.reply(200, "application/json", body)
            if path == "/":
                path = "/index.html"
            full = os.path.realpath(os.path.join(DIST, path.lstrip("/")))
            if not full.startswith(os.path.realpath(DIST) + os.sep) or \
                    not os.path.isfile(full):
                return self.reply(404, "text/plain", b"not found")
            with open(full, "rb") as fh:
                body = fh.read()
            self.reply(200, TYPES.get(os.path.splitext(full)[1],
                                      "application/octet-stream"), body)

        def do_PUT(self):
            if not self.trusted(changes=True):
                return
            if not self.logged_in():
                return self.json(401, {"error": "please log in"})
            if self.path != "/api/settings":
                return self.reply(404, "text/plain", b"not found")
            data = self.body_json()
            if data is None:
                return
            p = settings_path()
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p + ".tmp", "w") as fh:
                json.dump(data, fh, indent=2)
            os.replace(p + ".tmp", p)
            self.reply(204, "text/plain", b"")

        def reply(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)

        def upgrade(self):
            """Answer the WebSocket handshake; the socket is ours now."""
            k = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(
                hashlib.sha1(k.encode() + WS_GUID).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            self.wfile.flush()
            sock = self.connection
            # A phone that sleeps or changes network leaves a connection
            # open on our side only: keepalive finds it, and a send that
            # cannot complete in 30 s gives up instead of stalling every
            # page's stream (audit 4). recv() wakes every 30 s to ping.
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            for opt, val in (("TCP_KEEPIDLE", 30), ("TCP_KEEPINTVL", 10),
                             ("TCP_KEEPCNT", 3)):
                if hasattr(socket, opt):
                    sock.setsockopt(socket.IPPROTO_TCP,
                                    getattr(socket, opt), val)
            sock.settimeout(30)
            return WebSocket(sock)

        def messages(self, ws):
            """The page's JSON messages, until it goes."""
            try:
                while True:
                    text = ws.recv()
                    if text is None:
                        return
                    try:
                        msg = json.loads(text)
                    except ValueError:
                        continue
                    if isinstance(msg, dict):
                        yield msg
            except (OSError, ConnectionError):
                return

        def websocket_logs(self):
            """The Developer page's log view: join the shared streams."""
            hub = self.machine().hub
            ws = self.upgrade()
            hub.join(ws)
            try:
                for msg in self.messages(ws):
                    t = msg.get("t")
                    if t == "start" and msg.get("stream") in ("debug", "snoop"):
                        hub.start_stream(msg["stream"])
                    elif t == "stop" and msg.get("stream") in ("debug", "snoop"):
                        hub.stop_stream(msg["stream"])
                    elif t == "snoop":
                        hub.set_snoop(str(msg.get("task", ""))[:60],
                                      bool(msg.get("entry")))
            finally:
                ws.closed = True
                hub.leave(ws)
                self.close_connection = True

        def websocket_run(self):
            """Run one command at a time, its output as it comes."""
            mach = self.machine()
            target, key = mach.target, mach.key
            ws = self.upgrade()
            running = {}

            def run(cmd, max_time):
                c = None
                try:
                    c = w.Conn(target[0], target[1], key)
                    rc = w.do_run(c, cmd, max_time=max_time,
                                  sink=lambda t: ws.send_text({"t": "out",
                                                               "text": t}))
                    ws.send_text({"t": "exit", "rc": rc})
                except w.TimeLimit as exc:
                    ws.send_text({"t": "error", "msg": str(exc),
                                  "timeout": True})
                except (OSError, w.WasabiError) as exc:
                    ws.send_text({"t": "error", "msg": str(exc)})
                finally:
                    if c:
                        c.close()
                    running.pop("cmd", None)

            try:
                for msg in self.messages(ws):
                    if msg.get("t") == "run" and "cmd" not in running:
                        cmd = str(msg.get("cmd", "")).strip()
                        if not cmd:
                            continue
                        running["cmd"] = cmd
                        try:
                            mt = float(msg.get("max") or 0) or None
                        except (TypeError, ValueError):
                            mt = None
                        # a page that goes away does not leave a command
                        # running unbounded on the Amiga
                        threading.Thread(target=run, daemon=True, args=(
                            cmd, mt or 600.0)).start()
                    elif msg.get("t") == "stop" and "cmd" in running:
                        # Ctrl-C it, the way run --max-time does
                        try:
                            c = w.Conn(target[0], target[1], key)
                            try:
                                w.stop_command(c, running["cmd"])
                            finally:
                                c.close()
                        except (OSError, w.WasabiError):
                            pass
            finally:
                ws.closed = True
                self.close_connection = True

        def websocket_hello(self):
            """The app keeps this open for as long as its window exists:
            it, and nothing else, says whether the window is still there
            (the live screen comes and goes as the page changes)."""
            ws = self.upgrade()
            server_state.page_opened()
            try:
                for _ in self.messages(ws):
                    pass
            finally:
                ws.closed = True
                server_state.page_closed()
                self.close_connection = True

        def websocket(self):
            mach = self.machine()
            ws = self.upgrade()
            session = Session(w, mach.target, mach.key, ws)
            worker = threading.Thread(target=session.run, daemon=True)
            worker.start()
            try:
                while True:
                    text = ws.recv()
                    if text is None:
                        break
                    try:
                        msg = json.loads(text)
                    except ValueError:
                        continue
                    if isinstance(msg, dict):
                        session.inbox.put(msg)
            except (OSError, ConnectionError):
                pass
            finally:
                session.stop()
                worker.join(timeout=3)
                ws.closed = True
                self.close_connection = True

    return Handler


class ServerState:
    """Counts open pages, so the bridge can end when the window does."""
    def __init__(self):
        self.lock = threading.Lock()
        self.pages = 0
        self.ever = False
        self.empty_since = None

    def page_opened(self):
        with self.lock:
            self.pages += 1
            self.ever = True
            self.empty_since = None

    def page_closed(self):
        with self.lock:
            self.pages -= 1
            if self.pages == 0:
                self.empty_since = time.time()

    def idle_for(self):
        with self.lock:
            if not self.ever or self.pages or self.empty_since is None:
                return 0
            return time.time() - self.empty_since


def open_window(url, kiosk=False):
    """An app-style window where the browser has one: Chromium-family
    --app gives a window with no tabs or address bar. Firefox has no
    such mode, so it gets a new window (or --kiosk, full screen)."""
    for name in ("chromium", "chromium-browser", "google-chrome-stable",
                 "google-chrome", "brave", "brave-browser"):
        exe = shutil.which(name)
        if exe:
            args = [exe, "--app=" + url]
            if kiosk:
                args.append("--kiosk")
            break
    else:
        exe = shutil.which("firefox")
        if not exe:
            return False
        args = [exe, "--kiosk", url] if kiosk else [exe, "--new-window", url]
    subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)
    return True


def app_window(url, title, kiosk):
    """Open the page in a window of its own - no tabs, no address bar,
    and no browser shortcuts in the way (Ctrl+W goes to the Amiga) -
    through pywebview and whatever web engine the system has (WebKitGTK
    on this PC once `webkit2gtk-4.1` is installed; Qt WebEngine works
    too). Blocks until the window is closed and returns True; returns
    False at once when there is no such engine, and the caller falls
    back to a browser. WASABI_WEBVIEW_GUI picks a backend ("gtk"/"qt")."""
    try:
        import webview
    except ImportError:
        return False
    try:
        win = webview.create_window(title, url, width=1400, height=900,
                                    min_size=(800, 560),
                                    background_color="#000000",
                                    fullscreen=kiosk)
        close_after = float(os.environ.get("WASABI_WEBVIEW_CLOSE_AFTER", 0))
        if close_after:                 # tests: close it as a person would
            threading.Timer(close_after, win.destroy).start()
        webview.start(gui=os.environ.get("WASABI_WEBVIEW_GUI") or None,
                      private_mode=False)
    except Exception as exc:            # no usable engine: say so, fall back
        print("no app window (%s) - using the browser" % exc, file=sys.stderr)
        return False
    return True


STAMP = os.path.join(HERE, ".deployed")


def read_stamp():
    try:
        with open(STAMP) as fh:
            return fh.read().strip()
    except OSError:
        return ""


def watch_deploys(fleet):
    """Updates that apply themselves (the server on the NAS): the deploy
    script copies the program, then writes .deployed LAST. When that
    stamp changes, this process ends, and Docker's restart policy
    (unless-stopped restarts any exit) starts the container again on the
    new code. Watching only the stamp means a restart never happens
    halfway through a copy. Open pages see the new version and reload."""
    version = read_stamp()
    for m in fleet.machines:
        m.api.version = version

    def loop():
        while True:
            time.sleep(15)
            now = read_stamp()
            if now and now != version:
                print("a new version was deployed (%s) - restarting to take "
                      "it" % now, file=sys.stderr)
                time.sleep(2)           # let the copy settle, answers finish
                os._exit(0)
    threading.Thread(target=loop, daemon=True).start()


def serve(w, args, target):
    if not os.path.isfile(os.path.join(DIST, "index.html")):
        raise w.WasabiError(
            "the view's page is not built - run: cd %s/view && npm install "
            "&& npm run build" % HERE)
    state = ServerState()
    from wasabi_api import Api, Auth
    server = getattr(args, "mode", "") == "server"
    auth = Auth(os.path.join(os.environ.get(
        "XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "wasabi")) \
        if server else None
    root = getattr(args, "files", None) if server else None
    if root:
        os.makedirs(root, exist_ok=True)
    from wasabi_fleet import Fleet
    cfg_dir = os.path.join(os.environ.get(
        "XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "wasabi")
    fleet = Fleet(w, target, args.key, getattr(args, "protect", set()),
                  root, cfg_dir)
    if server:                          # history and alerts: the NAS only,
        from wasabi_monitor import Monitor   # for its first machine
        m = fleet.first
        m.api.monitor = Monitor(w, m.target, m.key, cfg_dir)
        m.api.monitor.start()
        from wasabi_backup import Backup
        m.api.backup = Backup(w, m, root, cfg_dir, m.api.monitor)
        m.api.backup.start()
    handler = make_handler(w, fleet, state, args.view_port, args.listen, auth)
    try:
        httpd = ThreadingHTTPServer((args.listen, args.view_port), handler)
    except OSError as exc:
        raise w.WasabiError("cannot listen on %s:%d - %s (another 'wasabi "
                            "view' running? --port picks another)"
                            % (args.listen, args.view_port, exc.strerror
                               or exc))
    httpd.daemon_threads = True
    url = "http://%s:%d/" % ("127.0.0.1" if args.listen in ("0.0.0.0", "")
                             else args.listen, args.view_port)
    if getattr(args, "mode", "view") == "view":
        url += "?mode=view"          # just the screen; desktop gets it all
    print("wasabi view on %s - the Amiga at %s:%d%s" % (
        url, target[0], target[1],
        "" if args.stay else "; closes when its window does"),
        file=sys.stderr)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    if server:
        watch_deploys(fleet)
        print("serving with a login%s; files in %s" % (
            " (the first visitor chooses the password)"
            if auth.needs_setup() else "", root), file=sys.stderr)
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass
        httpd.shutdown()
        return 0
    if not args.no_browser and not getattr(args, "browser", False):
        title = "Wasabi" if getattr(args, "mode", "view") != "view" \
            else "Wasabi view"
        if app_window(url, title, args.kiosk):
            httpd.shutdown()            # the window was closed: done
            return 0
    if not args.no_browser and not open_window(url, args.kiosk):
        print("no browser found - open %s yourself" % url, file=sys.stderr)
    try:
        while True:
            time.sleep(0.5)
            # A reload closes and reopens the page within a moment, so
            # wait a few seconds before deciding the window is gone.
            if not args.stay and state.idle_for() > 5:
                break
    except KeyboardInterrupt:
        print(file=sys.stderr)
    httpd.shutdown()
    return 0
