"""More than one Amiga in one app: the machines the bridge knows.

Each machine has its own API object (files, health, screenshots...) and
its own log hub (the daemon gives each stream to one listener per
machine). A page picks a machine with `?m=<id>` on every call; without
one, the first machine - the one `wasabi` itself would find - is used.

The list lives in the config folder as machines.json (private: a
machine may carry its own key); the first machine is written there the
first time. Imported by wasabi_view.py; `w` is the wasabi module.
"""

import json
import os
import re
import threading

from wasabi_api import Api, ApiError
from wasabi_logs import LogHub


class Machine:
    def __init__(self, fleet, d):
        self.id = d["id"]
        self.name = d.get("name") or d["id"]
        # "auto": wherever wasabi finds it this time (--host, the cache,
        # discovery) - the A1200's DHCP address has changed before, and
        # a machine list holding the old one would have lost it.
        self.auto = bool(d.get("auto"))
        if self.auto:
            self.host, self.port = fleet.found
        else:
            self.host = d["host"]
            self.port = int(d.get("port", 1234))
        self.key = d.get("key") or fleet.default_key
        self.target = (self.host, self.port)
        self.api = Api(fleet.w, self.target, self.key, fleet.protect,
                       root=fleet.root)
        self.hub = LogHub(fleet.w, self.target, self.key)

    def public(self):
        return {"id": self.id, "name": self.name, "host": self.host,
                "port": self.port, "auto": self.auto}


class Fleet:
    def __init__(self, w, first_target, default_key, protect, root, folder):
        self.w, self.default_key = w, default_key
        self.found = first_target
        self.protect, self.root = protect, root
        self.path = os.path.join(folder, "machines.json")
        self.lock = threading.Lock()
        self.machines = []
        try:
            with open(self.path) as fh:
                saved = json.load(fh)
        except (OSError, ValueError):
            saved = []
        for d in saved:
            try:
                self.machines.append(Machine(self, d))
            except (KeyError, ValueError):
                continue
        if not self.machines:
            self.machines.append(Machine(self, {
                "id": "amiga", "name": "Amiga", "auto": True}))
            self._save()

    def _save(self):
        rows = []
        for m in self.machines:
            d = m.public()
            if m.auto:
                d = {"id": m.id, "name": m.name, "auto": True}
            if m.key != self.default_key:
                d["key"] = m.key
            rows.append(d)
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        from wasabi_api import write_private
        write_private(self.path, json.dumps(rows, indent=2))

    def get(self, mid):
        with self.lock:
            for m in self.machines:
                if m.id == mid:
                    return m
            return self.machines[0]

    @property
    def first(self):
        return self.machines[0]

    def status(self, m):
        """Is it there, and what runs on it - a short handshake."""
        try:
            c = self.w.Conn(m.host, m.port, m.key, timeout=1.5)
        except (OSError, self.w.WasabiError):
            return {"online": False, "banner": ""}
        banner = c.banner
        c.close()
        return {"online": True, "banner": banner}

    def listing(self):
        with self.lock:
            ms = list(self.machines)
        out = [dict(m.public(), **{"online": False, "banner": ""}) for m in ms]
        threads = []
        for i, m in enumerate(ms):
            def check(i=i, m=m):
                out[i].update(self.status(m))
            t = threading.Thread(target=check)
            t.start()
            threads.append(t)
        for t in threads:
            t.join(3)
        return {"machines": out}

    def discover(self):
        """Every wasabid this network answers for, marked if known."""
        found = self.w.discover(timeout=0.8)
        known = {(m.host, m.port) for m in self.machines}
        return {"found": [{"host": h, "port": p, "name": n, "banner": b,
                           "known": (h, p) in known}
                          for (h, p), (n, b) in sorted(found.items())]}

    def change(self, body):
        act = body.get("action")
        with self.lock:
            if act == "add":
                host = str(body.get("host", "")).strip()
                if not re.match(r"^[A-Za-z0-9._-]+$", host):
                    raise ApiError("give the machine's address, like "
                                   "192.168.1.20 or 127.0.0.1")
                name = str(body.get("name") or host).strip()[:40]
                base = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") \
                    or "amiga"
                mid, n = base, 2
                while any(m.id == mid for m in self.machines):
                    mid, n = "%s-%d" % (base, n), n + 1
                d = {"id": mid, "name": name, "host": host,
                     "port": int(body.get("port") or 1234)}
                if body.get("key"):
                    d["key"] = str(body["key"])
                self.machines.append(Machine(self, d))
            elif act == "remove":
                if len(self.machines) == 1:
                    raise ApiError("keep at least one machine")
                self.machines = [m for m in self.machines
                                 if m.id != body.get("id")]
            elif act == "rename":
                for m in self.machines:
                    if m.id == body.get("id"):
                        m.name = str(body.get("name", m.name)).strip()[:40] \
                            or m.name
            else:
                raise ApiError("no such change")
            self._save()
        return self.listing()
