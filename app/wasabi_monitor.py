"""History and alerts, for `wasabi serve` (the NAS, which is always on).

Every 30 s the monitor reads the Amiga's health and keeps it for a day
(SQLite, in the config folder), so the app can draw the last hour or
day, not only the three minutes a page has been open. It also watches
for what a person would want to hear about at once, and tells the phone
through ntfy (a notification server - on the NAS too - with an app for
Android):

  too hot          the SoC at or over the limit (default 75 C);
                   "cooled down" once 5 C under it again
  power / heat     the Pi's throttled flags: a problem now, and anything
                   newly recorded since the Pi started
  not answering    four readings in a row (two minutes) - and "back"
  a new guru       the daemon's T:lastguru changed

Each alert is also kept as an event (30 days) for the app to list.
Imported by wasabi_view.py; `w` is the wasabi module.
"""

import json
import os
import secrets
import sqlite3
import threading
import time
import urllib.request

EVERY = float(os.environ.get("WASABI_MONITOR_EVERY", 30))   # tests: faster
KEEP_READINGS = 26 * 3600
KEEP_EVENTS = 30 * 86400
DOWN_AFTER = 4                   # readings in a row: two minutes


class Monitor:
    def __init__(self, w, target, key, folder):
        self.w, self.target, self.key = w, target, key
        self.db_path = os.path.join(folder, "history.db")
        self.cfg_path = os.path.join(folder, "alerts.json")
        self.lock = threading.Lock()
        self.cfg = self._load_cfg()
        self.prev = None
        self.fails = 0
        self.down = False
        self.hot = False
        self.now_bits = 0
        self.ever_bits = None           # since-boot bits last seen
        self.guru = None                # last guru seen (None: not yet)
        db = self._db()
        db.execute("CREATE TABLE IF NOT EXISTS readings (ts REAL, up INTEGER,"
                   " temp REAL, mips REAL, core_v REAL, throttled INTEGER,"
                   " chip_free INTEGER, fast_free INTEGER)")
        db.execute("CREATE TABLE IF NOT EXISTS events (ts REAL, kind TEXT,"
                   " text TEXT)")
        db.execute("CREATE INDEX IF NOT EXISTS readings_ts ON readings(ts)")
        db.commit()
        db.close()

    # --- settings -----------------------------------------------------------

    def _load_cfg(self):
        cfg = {"enabled": True, "ntfy_url": "http://127.0.0.1:8090",
               "phone_url": "http://bytebandit:8090", "topic": "",
               "temp_c": 75.0}
        try:
            with open(self.cfg_path) as fh:
                cfg.update(json.load(fh))
        except (OSError, ValueError):
            pass
        if not cfg["topic"]:
            # A topic is the only key to an ntfy feed: make it unguessable.
            cfg["topic"] = "wasabi-" + secrets.token_hex(6)
            self._save_cfg(cfg)
        return cfg

    def _save_cfg(self, cfg):
        from wasabi_api import write_private
        write_private(self.cfg_path, json.dumps(cfg, indent=2))

    def settings(self):
        with self.lock:
            return dict(self.cfg)

    def set_settings(self, new):
        """Checked on a copy first: a temp_c of "abc" stored before
        float() failed used to break every later check (audit 4)."""
        from wasabi_api import ApiError
        with self.lock:
            cfg = dict(self.cfg)
            try:
                if "temp_c" in new:
                    cfg["temp_c"] = float(new["temp_c"])
                if "enabled" in new:
                    cfg["enabled"] = bool(new["enabled"])
                for k in ("ntfy_url", "phone_url"):
                    if k in new:
                        v = str(new[k]).strip()
                        if not v.startswith(("http://", "https://")):
                            raise ValueError(k)
                        cfg[k] = v
                if "topic" in new:
                    t = str(new["topic"]).strip()
                    if not t or "/" in t:
                        raise ValueError("topic")
                    cfg["topic"] = t
            except (ValueError, TypeError):
                raise ApiError("those alert settings do not make sense")
            self.cfg = cfg
            self._save_cfg(self.cfg)
            return dict(self.cfg)

    # --- the loop -----------------------------------------------------------

    def start(self):
        threading.Thread(target=self._loop, daemon=True).start()

    def _db(self):
        return sqlite3.connect(self.db_path, timeout=30)

    def _loop(self):
        while True:
            t0 = time.time()
            try:
                self.tick()
            except Exception as exc:     # the monitor must never die
                print("[monitor] %s" % exc)
            time.sleep(max(1.0, EVERY - (time.time() - t0)))

    def tick(self):
        w = self.w
        f, guru = None, None
        try:
            # A silent Amiga must cost a check its timeout, not the
            # client's default 15 s, or four failures take a minute.
            c = w.Conn(self.target[0], self.target[1], self.key,
                       timeout=max(0.5, min(10.0, EVERY * 0.5)))
            try:
                cur = w.health_read(c)
                f = w.health_facts(cur, self.prev)
                self.prev = cur
                guru = w.last_guru(c)
            finally:
                c.close()
        except (OSError, w.WasabiError):
            f = None
        now = time.time()
        db = self._db()
        try:
            if f is None:
                db.execute("INSERT INTO readings (ts, up) VALUES (?, 0)", (now,))
            else:
                th = int(f.get("throttled_raw", "0"), 16) \
                    if f.get("throttled_raw") else None
                db.execute("INSERT INTO readings VALUES (?,1,?,?,?,?,?,?)", (
                    now, f.get("temp_c"), f.get("mips_68k"), f.get("core_v"),
                    th, f.get("chip_free_kb"), f.get("fast_free_kb")))
            db.execute("DELETE FROM readings WHERE ts < ?",
                       (now - KEEP_READINGS,))
            db.execute("DELETE FROM events WHERE ts < ?", (now - KEEP_EVENTS,))
            db.commit()
        finally:
            db.close()
        self._watch(f, guru)

    def _watch(self, f, guru):
        if f is None:
            self.fails += 1
            if self.fails >= DOWN_AFTER and not self.down:
                self.down = True
                self.alert("down", "The Amiga stopped answering",
                           "No answer for two minutes - frozen, switched "
                           "off, or off the network.", priority="high",
                           tags="warning")
            return
        if self.down:
            self.alert("up", "The Amiga is back",
                       "It answers again (%s)." % f.get("banner", ""),
                       tags="white_check_mark")
        self.fails = 0
        self.down = False

        limit = self.settings()["temp_c"]
        t = f.get("temp_c")
        if t is not None:
            if t >= limit and not self.hot:
                self.hot = True
                self.alert("hot", "The Amiga's Pi is hot: %.1f °C" % t,
                           "At or over the %.0f °C limit set in Wasabi. The "
                           "Pi slows itself down at %.0f °C." % (
                               limit, f.get("temp_max_c") or 85),
                           priority="high", tags="fire")
            elif self.hot and t < limit - 5:
                self.hot = False
                self.alert("cool", "Cooled down: %.1f °C" % t,
                           "Back under the limit.", tags="snowflake")

        if "problems_now" in f:
            raw = int(f["throttled_raw"], 16)
            now_bits, ever_bits = raw & 0xF, (raw >> 16) & 0xF
            if now_bits and not self.now_bits:
                self.alert("power", "Power or heat problem NOW",
                           ", ".join(f["problems_now"]) + ".",
                           priority="urgent", tags="zap")
            self.now_bits = now_bits
            if self.ever_bits is not None and ever_bits & ~self.ever_bits:
                self.alert("power-ever", "The Pi recorded a power or heat "
                           "problem", "Since the Pi started: %s." %
                           ", ".join(f["problems_since_boot"]),
                           priority="high", tags="zap")
            self.ever_bits = ever_bits

        if self.guru is None:
            self.guru = guru or ""       # what was there when we started
        elif guru and guru != self.guru:
            self.guru = guru
            self.alert("guru", "Guru Meditation on the Amiga", guru,
                       priority="high", tags="skull")

    # --- alerts and events --------------------------------------------------

    def alert(self, kind, title, text, priority="default", tags=""):
        now = time.time()
        db = self._db()
        try:
            db.execute("INSERT INTO events VALUES (?,?,?)",
                       (now, kind, "%s - %s" % (title, text)))
            db.commit()
        finally:
            db.close()
        cfg = self.settings()
        if cfg["enabled"] and cfg["ntfy_url"] and cfg["topic"]:
            self.push(cfg, title, text, priority, tags)

    @staticmethod
    def push(cfg, title, text, priority="default", tags=""):
        """Publish to ntfy as JSON (to the server's root, naming the
        topic inside): UTF-8 all through, so "°C" survives in a title,
        which a plain Title header would mangle."""
        body = {"topic": cfg["topic"], "title": title, "message": text,
                "priority": {"min": 1, "low": 2, "default": 3, "high": 4,
                             "urgent": 5}.get(priority, 3)}
        if tags:
            body["tags"] = tags.split(",")
        req = urllib.request.Request(
            cfg["ntfy_url"].rstrip("/") + "/",
            data=json.dumps(body).encode("utf-8"), method="POST",
            headers={"Content-Type": "application/json"})
        try:
            urllib.request.urlopen(req, timeout=10).read()
            return True
        except Exception as exc:         # a bad URL must not abort a check
            print("[monitor] ntfy: %s" % exc)
            return False

    def test(self):
        ok = self.push(self.settings(), "Wasabi test",
                       "If you can read this on your phone, alerts work.",
                       tags="tada")
        return {"sent": ok}

    # --- for the app ---------------------------------------------------------

    def history(self, hours):
        """Readings for the last `hours`, at most ~300 points (averaged
        into buckets), as [ts, up, temp, mips]."""
        since = time.time() - hours * 3600
        db = self._db()
        try:
            rows = db.execute("SELECT ts, up, temp, mips FROM readings "
                              "WHERE ts >= ? ORDER BY ts", (since,)).fetchall()
        finally:
            db.close()
        step = max(1, -(-len(rows) // 300))       # at most 300 points
        out = []
        for i in range(0, len(rows), step):
            chunk = rows[i:i + step]
            ups = [r for r in chunk if r[1]]
            if len(ups) < len(chunk):   # any time down shows as down:
                out.append([chunk[-1][0], 0, None, None])    # an outage
                continue                # must not be averaged away
            temps = [r[2] for r in ups if r[2] is not None]
            mips = [r[3] for r in ups if r[3] is not None]
            out.append([chunk[-1][0], 1,
                        round(sum(temps) / len(temps), 1) if temps else None,
                        round(sum(mips) / len(mips), 1) if mips else None])
        return {"every": EVERY * step, "readings": out}

    def events(self, n=20):
        db = self._db()
        try:
            rows = db.execute("SELECT ts, kind, text FROM events ORDER BY ts "
                              "DESC LIMIT ?", (n,)).fetchall()
        finally:
            db.close()
        return [{"ts": r[0], "kind": r[1], "text": r[2]} for r in rows]
