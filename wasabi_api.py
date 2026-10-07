"""The desktop app's API: what the page asks the bridge for, besides the
live screen. Imported by wasabi_view.py; the wasabi module is `w`.

Every call opens its own short connection to the daemon (a handshake is
~5 ms) rather than sharing one: a long copy must not hold up the health
readings, and the daemon serves up to eight connections.

The page lives on this PC only (127.0.0.1), but a browser visits other
sites too, and any of them could aim a request at 127.0.0.1. So:
requests must name this server as their Host (stops DNS rebinding),
anything that changes something must carry an X-Wasabi header (which a
foreign page cannot send without a CORS preflight this server never
answers), and the live screen's WebSocket must come from this origin.
"""

import hashlib
import hmac
import json
import os
import secrets
import struct
import threading
import time

SHOTS = os.path.join(os.path.expanduser("~"), "Pictures", "Wasabi")


class ApiError(Exception):
    def __init__(self, msg, code=400):
        Exception.__init__(self, msg)
        self.code = code


class Api:
    def __init__(self, w, target, key, protect, root=None, shots=None):
        self.w, self.target, self.key = w, target, key
        self.protect = protect
        # root: the server's files folder (Wasabi on the NAS) - the "PC"
        # side is then that folder and nothing above it. None: the whole
        # PC, as the desktop app on the user's own machine.
        self.root = os.path.realpath(root) if root else None
        self.monitor = None              # set by `wasabi serve`
        self.shots_dir = shots or (os.path.join(self.root, "Screenshots")
                                   if self.root else SHOTS)
        self.lock = threading.Lock()
        self.prev_health = None          # for the CPU meter's rate
        self.versions = None             # (time, {...}) - Version is a run
        self.volumes = None              # (time, [...]) - INFO is slowish

    # --- plumbing -----------------------------------------------------

    def conn(self):
        try:
            return self.w.Conn(self.target[0], self.target[1], self.key)
        except (OSError, self.w.WasabiError) as exc:
            raise ApiError("cannot reach the Amiga: %s" % exc, 503)

    def call(self, fn, *args):
        c = self.conn()
        try:
            return fn(c, *args)
        except self.w.WasabiError as exc:
            raise ApiError(str(exc), 409)
        except OSError as exc:
            raise ApiError("the connection failed: %s" % exc, 503)
        finally:
            c.close()

    # --- the overview -------------------------------------------------

    def health(self):
        def get(c):
            cur = self.w.health_read(c)
            with self.lock:
                prev, self.prev_health = self.prev_health, cur
            f = self.w.health_facts(cur, prev)
            f["last_guru"] = self.w.last_guru(c)
            f["banner"] = c.banner
            return f
        return self.call(get)

    def info(self, refresh=False):
        """Versions and volumes. Both are cached: Version costs the run
        slot, and INFO walks every volume (network ones included)."""
        now = time.time()

        def get(c):
            out = {"banner": c.banner, "caps": sorted(c.caps),
                   "host": "%s:%d" % self.target}
            if refresh or not self.versions or now - self.versions[0] > 300:
                text = []
                try:
                    self.w.do_run(c, "Version", sink=text.append, max_time=10)
                except self.w.WasabiError:
                    pass                 # a command is running: next time
                line = "".join(text).strip()
                if line:
                    self.versions = (now, {"system": line})
            out.update(self.versions[1] if self.versions else {})
            if refresh or not self.volumes or now - self.volumes[0] > 30:
                c.send(self.w.INFO)
                text = []
                self.w.drain_to_end(c, text.append)
                self.volumes = (now, parse_info(
                    b"".join(text).decode("latin-1")))
            out.update(self.volumes[1])
            return out
        return self.call(get)

    # --- files --------------------------------------------------------

    def amiga_ls(self, path):
        def get(c):
            c.send(self.w.LS, self.w.pack_str(path))
            text = []
            self.w.drain_to_end(c, text.append)
            rows = []
            for line in b"".join(text).decode("latin-1").splitlines():
                parts = line.split(" ", 6)
                if len(parts) < 7:
                    continue
                kind, size, bits, days, mins, ticks, name = parts
                rows.append({"name": name, "dir": kind == "d",
                             "size": None if kind == "d" else int(size),
                             "date": self.w.amiga_date(int(days), int(mins),
                                                       int(ticks))})
            rows.sort(key=lambda e: (not e["dir"], e["name"].lower()))
            return {"path": path, "entries": rows}
        return self.call(get)

    def real(self, path):
        """A path from the page -> a real path on this machine. Under a
        root, the page's paths are relative to it ("/" is the root) and
        nothing outside it can be named, symlinks included."""
        if self.root is None:
            return os.path.abspath(os.path.expanduser(path or "~"))
        full = os.path.realpath(os.path.join(self.root,
                                             (path or "/").lstrip("/")))
        if full != self.root and not full.startswith(self.root + os.sep):
            raise ApiError("that is outside the Wasabi folder", 403)
        return full

    def shown(self, real):
        """A real path -> the path the page shows."""
        if self.root is None:
            return real
        rel = os.path.relpath(real, self.root)
        return "/" if rel == "." else "/" + rel

    def local_ls(self, path):
        path = self.real(path)
        try:
            names = os.listdir(path)
        except OSError as exc:
            raise ApiError("cannot list %s: %s" % (path, exc.strerror))
        rows = []
        for n in names:
            if n.startswith("."):
                continue                 # hidden files stay hidden
            full = os.path.join(path, n)
            try:
                st = os.stat(full)
            except OSError:
                continue
            isdir = os.path.isdir(full)
            rows.append({"name": n, "dir": isdir,
                         "size": None if isdir else st.st_size,
                         "date": time.strftime("%Y-%m-%d %H:%M",
                                               time.localtime(st.st_mtime))})
        rows.sort(key=lambda e: (not e["dir"], e["name"].lower()))
        parent = os.path.dirname(path)
        if self.root is not None and path == self.root:
            parent = path
        return {"path": self.shown(path), "parent": self.shown(parent),
                "entries": rows}

    def guard(self, path, force, verb):
        place = self.w.system_place(path, self.protect)
        if place and not force:
            raise ApiError("%s is in %s, a system place - a mistake there "
                           "can stop the Amiga booting" % (path, place),
                           428)                 # the page asks, then retries

    def to_amiga(self, local_paths, amiga_dir, force=False):
        """Copy PC files and folders into an Amiga drawer."""
        copied = []

        def put_tree(c, src, dst):
            if os.path.isdir(src):
                c.send(self.w.MKDIR, self.w.pack_str(dst))
                try:
                    c.expect_ok()
                except self.w.WasabiError:
                    pass                 # it may exist already
                for n in sorted(os.listdir(src)):
                    if not n.startswith("."):
                        put_tree(c, os.path.join(src, n), amiga_join(dst, n))
            else:
                self.w.do_put(c, src, dst, quiet=True)
                copied.append(dst)

        local_paths = [self.real(p) for p in local_paths]
        for src in local_paths:
            self.guard(amiga_join(amiga_dir, os.path.basename(src)), force,
                       "write")
        self.call(lambda c: [put_tree(c, s, amiga_join(amiga_dir,
                                                       os.path.basename(s)))
                             for s in local_paths])
        return {"copied": copied}

    def to_pc(self, amiga_paths, local_dir):
        """Copy Amiga files and drawers into a PC folder."""
        local_dir = self.real(local_dir)
        copied = []

        def get_tree(c, src, dst, isdir):
            if isdir:
                os.makedirs(dst, exist_ok=True)
                for e in self.amiga_ls(src)["entries"]:
                    get_tree(c, amiga_join(src, e["name"]),
                             os.path.join(dst, e["name"]), e["dir"])
            else:
                tmp = dst + ".wasabi-part"
                with open(tmp, "wb") as fh:
                    c.send(self.w.GET, self.w.pack_str(src))
                    self.w.drain_to_end(c, fh.write)
                os.replace(tmp, dst)
                copied.append(self.shown(dst))

        def run(c):
            for item in amiga_paths:
                name = amiga_base(item["path"])
                get_tree(c, item["path"], os.path.join(local_dir, name),
                         item.get("dir", False))
        self.call(run)
        return {"copied": copied}

    def amiga_mkdir(self, path):
        def do(c):
            c.send(self.w.MKDIR, self.w.pack_str(path))
            c.expect_ok()
            return {"ok": True}
        return self.call(do)

    def amiga_delete(self, path, force=False, isdir=False):
        """Delete a file - or a drawer and everything in it: AmigaDOS
        only deletes empty drawers, so the contents go first. The page
        has asked the person before calling this."""
        self.guard(path, force, "delete")

        def remove(c, p, d):
            if d:
                for e in self.amiga_ls(p)["entries"]:
                    remove(c, amiga_join(p, e["name"]), e["dir"])
            c.send(self.w.DEL, self.w.pack_str(p))
            c.expect_ok()

        self.call(lambda c: remove(c, path, isdir))
        return {"ok": True}

    def local_mkdir(self, path):
        try:
            os.makedirs(self.real(path), exist_ok=False)
        except OSError as exc:
            raise ApiError("cannot make %s: %s" % (path, exc.strerror))
        return {"ok": True}

    # --- screenshots --------------------------------------------------

    def grab(self):
        def do(c):
            w_, h, out_h, pixels, _, _ = self.w.grab_pixels(c)
            os.makedirs(self.shots_dir, exist_ok=True)
            name = "amiga-%s.png" % time.strftime("%Y%m%d-%H%M%S")
            n = 1
            while os.path.exists(os.path.join(self.shots_dir, name)):
                n += 1
                name = "amiga-%s-%d.png" % (time.strftime("%Y%m%d-%H%M%S"), n)
            self.w.write_png(os.path.join(self.shots_dir, name), w_, out_h, pixels)
            return {"name": name, "width": w_, "height": out_h}
        return self.call(do)

    def shots(self):
        try:
            names = [n for n in os.listdir(self.shots_dir) if n.endswith(".png")]
        except OSError:
            names = []
        out = []
        for n in sorted(names, reverse=True):
            st = os.stat(os.path.join(self.shots_dir, n))
            out.append({"name": n, "size": st.st_size,
                        "date": time.strftime("%Y-%m-%d %H:%M",
                                              time.localtime(st.st_mtime))})
        return {"folder": self.shown(self.shots_dir) if self.root
                else self.shots_dir, "shots": out}

    def shot_path(self, name):
        if "/" in name or not name.endswith(".png") or name.startswith("."):
            raise ApiError("no such screenshot", 404)
        p = os.path.join(self.shots_dir, name)
        if not os.path.isfile(p):
            raise ApiError("no such screenshot", 404)
        return p

    def shot_delete(self, name):
        os.remove(self.shot_path(name))
        return {"ok": True}

    # --- the machine --------------------------------------------------

    def reboot(self):
        def do(c):
            self.w.do_reboot(c)
            return {"ok": True}
        return self.call(do)

    # --- the Developer page ---------------------------------------------

    def ps(self):
        def get(c):
            rows, has_free = self.w.ps_rows(c)
            return {"tasks": [{
                "addr": r[0], "kind": "process" if r[1] == "p" else "task",
                "pri": r[2], "state": r[3], "stack": r[4],
                "free": r[5] if r[5] >= 0 else None,
                "cli": r[6] if r[6] >= 0 else None, "name": r[7],
                "command": r[8],
                "tight": bool(r[5] >= 0 and self.w.stack_tight(r[5], r[4]))}
                for r in rows], "has_free": has_free}
        return self.call(get)

    def kill(self, addr, force=False):
        if not str(addr).lower().startswith("0x"):
            raise ApiError("name the task by its address")

        def do(c):
            c.send(self.w.KILL, struct.pack(">I", 1 if force else 0) +
                   self.w.pack_str(addr))
            c.expect_ok()
            return {"ok": True}
        return self.call(do)

    def screens(self):
        def get(c):
            screens, _ = self.w.list_windows(c)
            return {"screens": screens}
        return self.call(get)

    def screen_front(self, title):
        def do(c):
            c.send(self.w.SCREEN, struct.pack(">I", 0) + self.w.pack_str(title))
            self.w.drain_to_end(c, lambda b: None)
            return {"ok": True}
        return self.call(do)

    def clip(self):
        return self.call(lambda c: {"text": self.w.clip_get(c)})

    def clip_put(self, text, paste=False):
        """Put text on the Amiga's clipboard; paste=True also presses
        Right Amiga+V, which pastes it into the active window."""
        def do(c):
            self.w.clip_set(c, str(text)[:60000])
            if paste:
                c.send(self.w.KEY, struct.pack(">HHHHHHHHH", 0,
                                               0x67, 0x80, 0x34, 0x80,
                                               0xB4, 0x80, 0xE7, 0))
                c.expect_ok()
            return {"ok": True}
        return self.call(do)

    def need_monitor(self):
        if not self.monitor:
            raise ApiError("history and alerts live on the NAS's Wasabi "
                           "(wasabi serve)", 404)
        return self.monitor

    # --- editing an Amiga text file, with backups ----------------------------

    EDIT_MAX = 256 * 1024
    KEEP_BACKUPS = 20

    def backup_dir(self, path):
        """Where earlier versions of an Amiga file are kept: on this
        machine (the NAS's Wasabi folder for the server), never on the
        Amiga - no .bak files in S:."""
        base = os.path.join(self.root, "Backups") if self.root else \
            os.path.join(os.environ.get("XDG_DATA_HOME", os.path.expanduser(
                "~/.local/share")), "wasabi", "backups")
        safe = "".join(ch if ch.isalnum() or ch in "-._" else "_"
                       for ch in path)
        return os.path.join(base, safe)

    def _entry(self, c, path):
        """The LS entry for one path (its protection bits), or None."""
        parent, name = amiga_split(path)
        c.send(self.w.LS, self.w.pack_str(parent))
        text = []
        self.w.drain_to_end(c, text.append)
        for line in b"".join(text).decode("latin-1").splitlines():
            parts = line.split(" ", 6)
            if len(parts) == 7 and parts[6].lower() == name.lower():
                return {"dir": parts[0] == "d", "size": int(parts[1]),
                        "prot": int(parts[2])}
        return None

    def amiga_read(self, path):
        def get(c):
            e = self._entry(c, path)
            if e is None:
                raise ApiError("%s is not there" % path, 404)
            if e["dir"]:
                raise ApiError("%s is a drawer" % path)
            if e["size"] > self.EDIT_MAX:
                raise ApiError("%s is %d KB - too big to edit here" %
                               (path, e["size"] // 1024))
            data = self.w.fetch(c, path)
            if data is None:
                raise ApiError("cannot read %s" % path)
            if b"\0" in data:
                raise ApiError("%s is not a text file" % path)
            return {"path": path, "text": data.decode("latin-1"),
                    "size": len(data), "backups": self._backups(path)}
        return self.call(get)

    def _backups(self, path):
        d = self.backup_dir(path)
        try:
            names = sorted((n for n in os.listdir(d) if n.endswith(".txt")),
                           reverse=True)
        except OSError:
            names = []
        return names

    def amiga_write(self, path, text, force=False):
        """Save: back the current file up here first, then write it with
        the same protection bits (a plain upload would reset them and
        drop, say, the script bit of a file in S:)."""
        self.guard(path, force, "write")
        try:
            data = text.replace("\r\n", "\n").encode("latin-1")
        except UnicodeEncodeError as exc:
            raise ApiError("the Amiga cannot store %r (not Latin-1)"
                           % text[exc.start])

        def do(c):
            e = self._entry(c, path)
            backup = None
            if e is not None and not e["dir"]:
                old = self.w.fetch(c, path)
                if old is not None:
                    d = self.backup_dir(path)
                    os.makedirs(d, exist_ok=True)
                    backup = time.strftime("%Y-%m-%d_%H%M%S") + ".txt"
                    with open(os.path.join(d, backup), "wb") as fh:
                        fh.write(old)
                    for n in self._backups(path)[self.KEEP_BACKUPS:]:
                        os.remove(os.path.join(d, n))
            prot = e["prot"] if e is not None else self.w.PROT_DEFAULT
            self.w.put_bytes(c, data, path, prot)
            return {"ok": True, "backup": backup,
                    "backups": self._backups(path)}
        return self.call(do)

    def amiga_backup(self, path, name):
        if "/" in name or not name.endswith(".txt"):
            raise ApiError("no such backup", 404)
        try:
            with open(os.path.join(self.backup_dir(path), name), "rb") as fh:
                return {"text": fh.read().decode("latin-1")}
        except OSError:
            raise ApiError("no such backup", 404)

    # --- uploads from the page (drag and drop, the phone's picker) ---------

    UPLOAD_MAX = 512 * 1024 * 1024

    def upload(self, side, folder, name, data, force=False):
        if not name or "/" in name or ":" in name or name in (".", ".."):
            raise ApiError("a file name may not hold / or :")
        if side == "amiga":
            target = amiga_join(folder, name)
            self.guard(target, force, "write")
            self.call(lambda c: self.w.put_bytes(c, data, target))
            return {"ok": True, "path": target}
        d = self.real(folder)
        full = os.path.join(d, name)
        self.real(self.shown(full))            # inside the root, still
        with open(full + ".wasabi-part", "wb") as fh:
            fh.write(data)
        os.replace(full + ".wasabi-part", full)
        return {"ok": True, "path": self.shown(full)}

    def me(self):
        return {"mode": "server" if self.root else "desktop",
                "history": self.monitor is not None,
                "local_name": "NAS" if self.root else "This PC",
                "local_home": "/" if self.root else "~"}

    def dispatch(self, method, path, query, body):
        """Route one /api call; returns a JSON-able object."""
        q = lambda k, d="": query.get(k, [d])[0]     # noqa: E731
        routes = {
            ("GET", "/api/health"): lambda: self.health(),
            ("GET", "/api/info"): lambda: self.info(q("refresh") == "1"),
            ("GET", "/api/amiga/ls"): lambda: self.amiga_ls(q("path")),
            ("GET", "/api/local/ls"): lambda: self.local_ls(q("path")),
            ("GET", "/api/shots"): lambda: self.shots(),
            ("GET", "/api/me"): lambda: self.me(),
            ("GET", "/api/ps"): lambda: self.ps(),
            ("GET", "/api/clip"): lambda: self.clip(),
            ("GET", "/api/amiga/read"): lambda: self.amiga_read(q("path")),
            ("GET", "/api/amiga/backup"): lambda: self.amiga_backup(
                q("path"), q("name")),
            ("POST", "/api/amiga/write"): lambda: self.amiga_write(
                body["path"], body["text"], body.get("force", False)),
            ("GET", "/api/history"): lambda: self.need_monitor().history(
                min(48.0, max(0.1, float(q("hours", "1"))))),
            ("GET", "/api/alerts"): lambda: {
                "settings": self.need_monitor().settings(),
                "events": self.need_monitor().events()},
            ("POST", "/api/alerts"): lambda: self.need_monitor().set_settings(
                body),
            ("POST", "/api/alerts/test"): lambda: self.need_monitor().test(),
            ("POST", "/api/clip"): lambda: self.clip_put(
                body["text"], body.get("paste", False)),
            ("GET", "/api/screens"): lambda: self.screens(),
            ("POST", "/api/kill"): lambda: self.kill(body["addr"],
                                                     body.get("force", False)),
            ("POST", "/api/screen/front"): lambda: self.screen_front(
                body["title"]),
            ("POST", "/api/copy/to-amiga"): lambda: self.to_amiga(
                body["paths"], body["dir"], body.get("force", False)),
            ("POST", "/api/copy/to-pc"): lambda: self.to_pc(
                body["items"], body["dir"]),
            ("POST", "/api/amiga/mkdir"): lambda: self.amiga_mkdir(
                body["path"]),
            ("POST", "/api/amiga/delete"): lambda: self.amiga_delete(
                body["path"], body.get("force", False), body.get("dir", False)),
            ("POST", "/api/local/mkdir"): lambda: self.local_mkdir(
                body["path"]),
            ("POST", "/api/grab"): lambda: self.grab(),
            ("POST", "/api/shots/delete"): lambda: self.shot_delete(
                body["name"]),
            ("POST", "/api/reboot"): lambda: self.reboot(),
        }
        fn = routes.get((method, path))
        if not fn:
            raise ApiError("no such call", 404)
        try:
            return fn()
        except (KeyError, TypeError):
            raise ApiError("bad request")


def amiga_join(d, name):
    if not d:
        return name
    return d + name if d.endswith((":", "/")) else d + "/" + name


def amiga_split(p):
    """'S:User-Startup' -> ('S:', 'User-Startup'); 'Work:a/b' -> ('Work:a', 'b')."""
    i = max(p.rfind("/"), p.rfind(":"))
    parent = p[:i + 1] if p[i:i + 1] == ":" else p[:i]
    return parent, p[i + 1:]


def amiga_base(p):
    return p.rstrip("/").replace(":", "/").split("/")[-1] or p


def parse_info(text):
    """INFO's text: the volume lines and the exec version."""
    vols, out = [], {}
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("exec.library"):
            out["exec"] = s.split(" ", 1)[1] if " " in s else s
        parts = s.split()
        # "Work:  2860 MB total  2742 MB free  4% used" - names may hold spaces
        if "total" in parts and "free" in parts and s.endswith("used"):
            try:
                i = parts.index("MB")
                name = " ".join(parts[:i - 1]).rstrip(":")
                total = int(parts[i - 1])
                free = int(parts[parts.index("free") - 2])
                vols.append({"name": name, "total_mb": total,
                             "free_mb": free})
            except (ValueError, IndexError):
                continue
    out["volumes"] = vols
    return out


class Auth:
    """The server's login (`wasabi serve`): one password, chosen by the
    first visitor - the person who has just set the server up - and kept
    as a salted scrypt hash. A login is a random session token in an
    HttpOnly, SameSite=Strict cookie, remembered for 30 days and kept on
    disk so a restart of the container does not log the phone out."""

    COOKIE = "wasabi_session"
    DAYS = 30

    def __init__(self, folder):
        os.makedirs(folder, exist_ok=True)
        self.pw_file = os.path.join(folder, "view-password")
        self.ss_file = os.path.join(folder, "view-sessions.json")
        self.lock = threading.Lock()
        try:
            with open(self.ss_file) as fh:
                self.sessions = {k: v for k, v in json.load(fh).items()
                                 if v > time.time()}
        except (OSError, ValueError):
            self.sessions = {}

    def needs_setup(self):
        return not os.path.isfile(self.pw_file)

    @staticmethod
    def _hash(pw, salt):
        return hashlib.scrypt(pw.encode(), salt=salt, n=2 ** 14, r=8, p=1,
                              dklen=32)

    def setup(self, pw):
        if not self.needs_setup():
            raise ApiError("a password is already set", 409)
        if len(pw) < 8:
            raise ApiError("choose at least 8 characters")
        salt = secrets.token_bytes(16)
        tmp = self.pw_file + ".tmp"
        with open(tmp, "w") as fh:
            fh.write("scrypt$%s$%s\n" % (salt.hex(), self._hash(pw, salt).hex()))
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.pw_file)
        return self._new_session()

    def login(self, pw):
        try:
            with open(self.pw_file) as fh:
                _, salt, want = fh.read().strip().split("$")
        except (OSError, ValueError):
            raise ApiError("no password is set yet", 409)
        if not hmac.compare_digest(self._hash(pw, bytes.fromhex(salt)).hex(),
                                   want):
            time.sleep(1.0)             # slow a guesser down
            raise ApiError("wrong password", 401)
        return self._new_session()

    def _new_session(self):
        token = secrets.token_urlsafe(32)
        with self.lock:
            self.sessions[token] = time.time() + self.DAYS * 86400
            self._save()
        return token

    def _save(self):
        tmp = self.ss_file + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(self.sessions, fh)
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.ss_file)

    def check(self, token):
        with self.lock:
            exp = self.sessions.get(token or "")
            return bool(exp and exp > time.time())

    def logout(self, token):
        with self.lock:
            if self.sessions.pop(token or "", None):
                self._save()
