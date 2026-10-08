"""A nightly backup of the Amiga's settings, for `wasabi serve` (the NAS).

Every night (03:30 by default) the drawers that decide whether the
Amiga boots and how it behaves - S:, ENVARC: and DEVS: - are copied into
<files>/Backups/Amiga/<date>/<S|ENVARC|DEVS>/..., and the last 30 nights
are kept. A file that is the same as the night before (same size, same
Amiga date) is not stored again: it is a hard link to last night's copy,
so thirty nights cost little more than one. Restoring is the Files page:
open the night on the NAS side, copy the file back (a system place asks
first).

A failed night is an alert (through the monitor's ntfy), and the app
shows the last run. Imported by wasabi_view.py; `w` is the wasabi module.
"""

import datetime
import json
import os
import shutil
import threading
import time

# The whole boot volume: AmigaOS is small (~24 MB used on the A1200),
# and SYS: holds C:, S:, LIBS:, DEVS:, Prefs and ENVARC: alike. SYS:
# follows whichever volume the Amiga booted from.
DEFAULTS = {"enabled": True, "time": "03:30", "keep": 30,
            "folders": ["SYS:"]}
MAX_DEPTH = 24                    # a drawer link pointing up must not loop


class Backup:
    def __init__(self, w, machine, root, folder, monitor=None):
        self.w, self.machine, self.monitor = w, machine, monitor
        self.base = os.path.join(root, "Backups", "Amiga")
        self.cfg_path = os.path.join(folder, "backup.json")
        self.state_path = os.path.join(folder, "backup-state.json")
        self.lock = threading.Lock()
        self.running = False
        self.cfg = dict(DEFAULTS)
        try:
            with open(self.cfg_path) as fh:
                self.cfg.update(json.load(fh))
        except (OSError, ValueError):
            pass
        try:
            with open(self.state_path) as fh:
                self.state = json.load(fh)
        except (OSError, ValueError):
            self.state = {}

    # --- for the app ----------------------------------------------------

    def info(self):
        try:
            nights = sorted((d for d in os.listdir(self.base)
                             if len(d) == 10 and d[4] == "-"), reverse=True)
        except OSError:
            nights = []
        return {"settings": dict(self.cfg), "last": self.state.get("last"),
                "running": self.running, "nights": nights,
                "folder": "/Backups/Amiga"}

    def set_settings(self, new):
        """Check every value on a copy, then take it: a value stored
        before it was checked (keep "x", folders 5, time "3") used to
        break every later night, silently (audit 4)."""
        from wasabi_api import ApiError
        cfg = dict(self.cfg)
        try:
            if "folders" in new:
                f = new["folders"]
                if isinstance(f, str):
                    f = f.split(",")
                if not isinstance(f, list) or not all(isinstance(x, str)
                                                      for x in f):
                    raise ValueError("folders")
                f = [x.strip() for x in f if x.strip()]
                cfg["folders"] = [x if ":" in x else x + ":" for x in f] \
                    or list(DEFAULTS["folders"])
            if "keep" in new:
                cfg["keep"] = max(1, min(365, int(new["keep"])))
            if "time" in new:
                h, m = str(new["time"]).strip().split(":")
                if not (0 <= int(h) < 24 and 0 <= int(m) < 60):
                    raise ValueError("time")
                cfg["time"] = "%02d:%02d" % (int(h), int(m))
            if "enabled" in new:
                cfg["enabled"] = bool(new["enabled"])
        except (ValueError, TypeError):
            raise ApiError("those settings do not make sense: give the time "
                           "as HH:MM, nights as a number, folders as names")
        self.cfg = cfg
        self._write(self.cfg_path, self.cfg)
        return self.info()

    def start_now(self):
        if self.running:
            return {"started": False, "running": True}
        threading.Thread(target=self.run, daemon=True).start()
        return {"started": True}

    # --- the night ---------------------------------------------------------

    def start(self):
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            time.sleep(30)
            try:
                if not self.cfg["enabled"] or self.running:
                    continue
                now = datetime.datetime.now()
                today = now.strftime("%Y-%m-%d")
                if now.strftime("%H:%M") >= self.cfg["time"] and \
                        self.state.get("day") != today:
                    self.run(scheduled=True)
            except Exception as exc:     # the scheduler must never die
                print("[backup] %s" % exc)

    def run(self, scheduled=False):
        """One backup. Only the scheduled run marks the night as done: a
        'Back up now' at 01:44 must not cancel that night's 03:30 run."""
        with self.lock:
            if self.running:
                return
            self.running = True
        t0 = time.time()
        today = datetime.datetime.now().strftime("%Y-%m-%d")
        target = os.path.join(self.base, today)
        work = target + ".partial"
        prev = self._previous(today)
        self.skipped = []
        files = linked = 0
        nbytes = 0
        try:
            for d in os.listdir(self.base) if os.path.isdir(self.base) else []:
                if d.endswith(".partial"):     # a night cut short earlier
                    shutil.rmtree(os.path.join(self.base, d),
                                  ignore_errors=True)
            c = self.w.Conn(self.machine.host, self.machine.port,
                            self.machine.key)
            try:
                for vol in self.cfg["folders"]:
                    name = vol.rstrip(":/").replace(":", "_") or "root"
                    f, l, b = self._copy_tree(c, vol, os.path.join(work, name),
                                              os.path.join(prev, name)
                                              if prev else None)
                    files, linked, nbytes = files + f, linked + l, nbytes + b
            finally:
                c.close()
            shutil.rmtree(target, ignore_errors=True)    # a second run today
            os.replace(work, target)
            self._prune()
            last = {"ok": True, "at": t0, "day": today, "files": files,
                    "new": files - linked, "bytes": nbytes,
                    "skipped": self.skipped[:20],
                    "seconds": round(time.time() - t0, 1)}
        except Exception as exc:         # any failure is a failed night,
            shutil.rmtree(work, ignore_errors=True)   # with its alert
            last = {"ok": False, "at": t0, "day": today, "error": str(exc)}
            if self.monitor:
                self.monitor.alert("backup", "The Amiga's backup failed",
                                   "Tonight's copy of %s did not finish: %s"
                                   % (", ".join(self.cfg["folders"]), exc),
                                   priority="high", tags="floppy_disk")
        day = today if scheduled else self.state.get("day")
        self.state = {"day": day, "last": last}
        try:
            self._write(self.state_path, self.state)
        finally:
            self.running = False

    def _copy_tree(self, c, amiga_dir, dest, prev_dir, depth=0):
        """Copy one drawer down; link files unchanged since last night.
        Returns (files, linked, bytes)."""
        if depth > MAX_DEPTH:
            self.skipped.append("%s (deeper than %d drawers - a drawer "
                                "link looping back?)" % (amiga_dir, MAX_DEPTH))
            return 0, 0, 0
        os.makedirs(dest, exist_ok=True)
        c.send(self.w.LS, self.w.pack_str(amiga_dir))
        text = []
        self.w.drain_to_end(c, text.append)
        files = linked = nbytes = 0
        manifest = {}
        old = {}
        if prev_dir:
            try:
                with open(os.path.join(prev_dir, ".wasabi-manifest.json")) as fh:
                    old = json.load(fh)
            except (OSError, ValueError):
                old = {}
        for line in b"".join(text).decode("latin-1").splitlines():
            parts = line.split(" ", 6)
            if len(parts) < 7:
                continue
            kind, size, _bits, days, mins, ticks, name = parts
            if name in (".", "..") or "/" in name:
                continue
            src = amiga_dir + name if amiga_dir.endswith((":", "/")) \
                else amiga_dir + "/" + name
            out = os.path.join(dest, name)
            if kind == "d":
                f, l, b = self._copy_tree(c, src, out, os.path.join(
                    prev_dir, name) if prev_dir else None, depth + 1)
                files, linked, nbytes = files + f, linked + l, nbytes + b
                continue
            stamp = "%s %s %s %s" % (size, days, mins, ticks)
            manifest[name] = stamp
            earlier = os.path.join(prev_dir, name) if prev_dir else None
            if earlier and old.get(name) == stamp and os.path.isfile(earlier):
                try:
                    os.link(earlier, out)      # unchanged: share last night's
                    files, linked = files + 1, linked + 1
                    continue
                except OSError:
                    pass                       # no hard links here: copy
            # One unreadable file (locked, or gone since the listing) is
            # skipped and named - it must not cost the whole night.
            try:
                with open(out + ".wasabi-part", "wb") as fh:
                    c.send(self.w.GET, self.w.pack_str(src))
                    self.w.drain_to_end(c, fh.write)
            except self.w.WasabiError as exc:
                os.remove(out + ".wasabi-part")
                self.skipped.append("%s (%s)" % (src, exc))
                manifest.pop(name, None)
                continue
            os.replace(out + ".wasabi-part", out)
            files += 1
            nbytes += int(size)
        self._write(os.path.join(dest, ".wasabi-manifest.json"), manifest)
        return files, linked, nbytes

    def _previous(self, today):
        try:
            nights = sorted(d for d in os.listdir(self.base)
                            if len(d) == 10 and d < today)
        except OSError:
            return None
        return os.path.join(self.base, nights[-1]) if nights else None

    def _prune(self):
        nights = sorted(d for d in os.listdir(self.base)
                        if len(d) == 10 and d[4] == "-")
        for d in nights[:-int(self.cfg["keep"])]:
            shutil.rmtree(os.path.join(self.base, d), ignore_errors=True)

    @staticmethod
    def _write(path, obj):
        tmp = path + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(obj, fh, indent=2)
        os.replace(tmp, path)
