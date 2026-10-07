"""The debug and snoop streams for the app's Developer page.

The daemon gives each stream to ONE subscriber at a time ("already in
use"), so the bridge holds one subscription per stream and shares it
with every open page: it starts when the first page asks and stops when
the last one leaves. A page that opens late gets the last lines first.

Lines go out whole, dressed the way the terminal shows them ("(err
205)" spelt out, alert codes named), and tagged - Wasabi's own traffic,
routine poll noise - so the page can hide them with a tick box instead
of the bridge deciding. Imported by wasabi_view.py; `w` is the wasabi
module.
"""

import collections
import socket
import struct
import threading
import time

KEEP = 2000                       # lines kept for a page that opens late
NAMES = {0: "debug", 1: "snoop"}


class LogHub:
    def __init__(self, w, target, key):
        self.w, self.target, self.key = w, target, key
        self.lock = threading.Lock()
        self.pages = set()                     # objects with send_text()
        self.history = collections.deque(maxlen=KEEP)
        self.workers = {}                      # stream name -> thread
        self.stop = {}                         # stream name -> Event
        self.state = {"debug": "off", "snoop": "off"}
        self.snoop_task = ""
        self.snoop_entry = False

    # --- pages ----------------------------------------------------------

    def join(self, page):
        with self.lock:
            self.pages.add(page)
            backlog = list(self.history)
            states = dict(self.state)
        for s, st in states.items():
            page.send_text({"t": "state", "stream": s, "state": st})
        page.send_text({"t": "backlog", "lines": backlog,
                        "snoop_task": self.snoop_task,
                        "snoop_entry": self.snoop_entry})

    def leave(self, page):
        with self.lock:
            self.pages.discard(page)
            empty = not self.pages
        if empty:
            for s in list(self.workers):
                self.stop_stream(s)

    def broadcast(self, obj):
        with self.lock:
            pages = list(self.pages)
        for p in pages:
            p.send_text(obj)

    def set_state(self, stream, state, msg=""):
        self.state[stream] = state
        self.broadcast({"t": "state", "stream": stream, "state": state,
                        "msg": msg})

    # --- streams --------------------------------------------------------

    def start_stream(self, stream):
        if stream in self.workers and self.workers[stream].is_alive():
            return
        ev = threading.Event()
        self.stop[stream] = ev
        t = threading.Thread(target=self._run, args=(stream, ev),
                             daemon=True)
        self.workers[stream] = t
        t.start()

    def stop_stream(self, stream):
        ev = self.stop.pop(stream, None)
        if ev:
            ev.set()
        self.workers.pop(stream, None)
        self.set_state(stream, "off")

    def set_snoop(self, task, entry):
        """New snoop options are the daemon's, per subscription: restart
        the snoop stream if it runs."""
        self.snoop_task, self.snoop_entry = task or "", bool(entry)
        if "snoop" in self.workers:
            self.stop_stream("snoop")
            self.start_stream("snoop")

    def _subscribe(self, c, stream):
        w = self.w
        if stream == "debug":
            c.send(w.DEBUG, struct.pack(">I", 0))
        else:
            c.send(w.SNOOP, struct.pack(">I", w.SNOOPF_ENTRY
                                        if self.snoop_entry else 0)
                   + w.pack_str(self.snoop_task))

    def _run(self, stream, ev):
        """Hold one subscription; reconnect while pages want it."""
        w = self.w
        partial = ""
        while not ev.is_set():
            try:
                c = w.Conn(self.target[0], self.target[1], self.key)
            except (OSError, w.WasabiError) as exc:
                self.set_state(stream, "waiting", "cannot reach the Amiga: %s"
                               % exc)
                ev.wait(3)
                continue
            try:
                self._subscribe(c, stream)
                c.sock.settimeout(1.0)          # to notice `ev` promptly
                self.set_state(stream, "on")
                silent = 0.0
                while not ev.is_set():
                    try:
                        tag, payload = c.recv()
                    except socket.timeout:
                        silent += 1.0
                        if silent >= 20:        # no heartbeat for 20 s
                            raise OSError("nothing heard for 20 s")
                        continue
                    silent = 0.0
                    if tag == w.ERR:
                        msg = c._errtext(payload)
                        busy = "already in use" in msg
                        self.set_state(stream, "busy" if busy else "error",
                                       "the %s stream is open somewhere "
                                       "else (a terminal?) - trying again"
                                       % stream if busy else msg)
                        ev.wait(5)
                        break
                    if tag != w.LOG:
                        continue
                    _, _seq = struct.unpack_from(">II", payload, 0)
                    text, _ = w.unpack_str(payload, 8)
                    if not text:
                        continue                # a heartbeat
                    buf = partial + text
                    lines = buf.split("\n")
                    partial = lines.pop()
                    for ln in lines:
                        self._line(stream, ln)
            except (OSError, w.WasabiError) as exc:
                if not ev.is_set():
                    self.set_state(stream, "waiting", "lost the Amiga (%s) - "
                                   "reconnecting" % exc)
                    ev.wait(2)
            finally:
                c.close()

    def _line(self, stream, raw):
        w = self.w
        text = w.dress_errors(raw.rstrip("\r"))
        if not text.strip():
            return
        item = {"s": stream, "ts": round(time.time(), 3), "text": text,
                "self": bool(w.WASABI_SELF_RE.search(text)),
                "noise": stream == "snoop" and bool(w.NOISE_RE.search(text))}
        with self.lock:
            self.history.append(item)
        self.broadcast({"t": "line", **item})
