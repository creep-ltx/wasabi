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
MIN_FRAME = 0.1                 # at most ~10 grabs a second
DEBUG = bool(os.environ.get("WASABI_VIEW_DEBUG"))   # log input to stderr


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

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("page closed")
            buf += chunk
        return buf

    def recv(self):
        """The next text message (str) or None when the page closes."""
        while True:
            b0, b1 = self._read(2)
            op, masked, n = b0 & 0x0F, b1 & 0x80, b1 & 0x7F
            if n == 126:
                (n,) = struct.unpack(">H", self._read(2))
            elif n == 127:
                (n,) = struct.unpack(">Q", self._read(8))
            mask = self._read(4) if masked else b"\0\0\0\0"
            data = bytearray(self._read(n))
            for i in range(n):
                data[i] ^= mask[i & 3]
            if op == 8:
                return None
            if op == 9:
                self._send(10, bytes(data))
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
            except OSError:
                self.closed = True

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
                    self.ws.send_text({"t": "status", "connected": True,
                                       "banner": conn.banner})
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
        self._drain_input(conn)
        self._grab(conn)
        # Wait out the rest of the frame - but wake for input at once.
        # What woke us is kept aside, in order, for the next drain: put
        # back on the queue it would go behind newer input, and a key
        # could reach the Amiga before the Right Amiga pressed with it
        # (seen on the A1200: Right Amiga+E arrived as E, then Amiga).
        left = MIN_FRAME - (time.time() - t0)
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
        # Only the last of a run of moves matters: the pointer goes
        # where it ended up, not through every point it passed.
        for i, m in enumerate(msgs):
            if m.get("t") == "move" and i + 1 < len(msgs) and \
                    msgs[i + 1].get("t") == "move":
                continue
            self._input(conn, m)

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
         ".woff2": "font/woff2", ".woff": "font/woff", ".ttf": "font/ttf"}


def make_handler(w, target, key, server_state):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):      # quiet: this is an app window
            pass

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/ws":
                return self.websocket()
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
            if self.path != "/api/settings":
                return self.reply(404, "text/plain", b"not found")
            n = int(self.headers.get("Content-Length", "0"))
            try:
                data = json.loads(self.rfile.read(n))
                if not isinstance(data, dict):
                    raise ValueError
            except ValueError:
                return self.reply(400, "text/plain", b"bad settings")
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

        def websocket(self):
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
            sock.settimeout(None)
            ws = WebSocket(sock)
            session = Session(w, target, key, ws)
            worker = threading.Thread(target=session.run, daemon=True)
            server_state.page_opened()
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
                server_state.page_closed()
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


def serve(w, args, target):
    if not os.path.isfile(os.path.join(DIST, "index.html")):
        raise w.WasabiError(
            "the view's page is not built - run: cd %s/view && npm install "
            "&& npm run build" % HERE)
    state = ServerState()
    handler = make_handler(w, target, args.key, state)
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
    print("wasabi view on %s - the Amiga at %s:%d%s" % (
        url, target[0], target[1],
        "" if args.stay else "; closes when its window does"),
        file=sys.stderr)
    if not args.no_browser and not open_window(url, args.kiosk):
        print("no browser found - open %s yourself" % url, file=sys.stderr)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
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
