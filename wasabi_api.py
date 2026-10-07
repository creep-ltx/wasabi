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

    def me(self):
        return {"mode": "server" if self.root else "desktop",
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
