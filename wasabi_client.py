"""wasabi_client - drive an Amiga from Python, through Wasabi.

The stable API for scripts and test rigs. The `wasabi` command's own
functions change whenever the command does; this module's names,
arguments and return values do not, except with a new major
API_VERSION. Reference: docs/api.md.

    import wasabi_client

    with wasabi_client.connect() as amiga:          # finds it, as `wasabi` does
        print(amiga.run("Version FULL").stdout)
        amiga.key_type("dir\\n")
        amiga.click(320, 100)
        amiga.input(["down 10 20", "move 200 20", "wait 5", "up"])
        amiga.grab().save("shot.png")
        amiga.ui_click("Save", window="Prefs")

It needs the `wasabi` script beside it (both come from the same
release). Python 3.8+, standard library only.
"""

import importlib.machinery
import importlib.util
import os
import sys
from collections import namedtuple

API_VERSION = "1.1"


def _load_cli():
    """The `wasabi` script beside this file, as a private module."""
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.environ.get("WASABI_SCRIPT") or os.path.join(here, "wasabi")
    if not os.path.isfile(path):
        raise ImportError("wasabi_client needs the 'wasabi' script beside "
                          "it (looked for %s)" % path)
    loader = importlib.machinery.SourceFileLoader("_wasabi_cli", path)
    spec = importlib.util.spec_from_loader("_wasabi_cli", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


_w = _load_cli()

# The errors, so `except wasabi_client.WasabiError` catches them all.
WasabiError = _w.WasabiError        # the command failed / the Amiga said no
TimeLimit = _w.TimeLimit            # max_time ran out
AmigaSilent = _w.AmigaSilent        # the Amiga stopped answering

RunResult = namedtuple("RunResult", "rc stdout stderr")
RexxResult = namedtuple("RexxResult", "rc rc2 result")
# The console's text (Zenix): lines without trailing spaces, the grid's
# size, cursor (row, column), whether it is the one shown; styles and
# colours (each row's bytes as hex) only when asked for, else None.
ConsoleText = namedtuple("ConsoleText", "lines rows cols cursor console "
                         "shown changes styles colours")
Entry = namedtuple("Entry", "name dir size protection date")


class Image:
    """A grab: width, height and RGB bytes (3 per pixel, rows top
    down). save() writes a PNG."""

    def __init__(self, width, height, rgb):
        self.width, self.height, self.rgb = width, height, rgb

    def pixel(self, x, y):
        i = (y * self.width + x) * 3
        return tuple(self.rgb[i:i + 3])

    def save(self, path):
        _w.write_png(path, self.width, self.height, self.rgb)
        return path

    def __repr__(self):
        return "<Image %dx%d>" % (self.width, self.height)


def connect(host=None, port=None, key=None, tls=None, native=False,
            timeout=None):
    """Connect to an Amiga; returns an Amiga. Anything not given comes
    from where `wasabi` takes it: WASABI_HOST/WASABI_PORT/WASABI_KEY,
    then ~/.config/wasabi/config, then discovery on the network.

    tls: None decides as `wasabi` does (encrypted when this computer has
    keys; never a silent downgrade), True insists, False never encrypts
    (the same as --plain). native: coordinates in the screen's own
    pixels rather than grab's (see docs/screen-and-gui.md#coordinates).
    timeout: seconds of silence before AmigaSilent (default 15)."""
    return Amiga(host, port, key, tls, native, timeout)


class Amiga:
    """One connection to one wasabid. Not thread-safe: one call at a
    time; open a second connection for parallel work."""

    def __init__(self, host=None, port=None, key=None, tls=None,
                 native=False, timeout=None):
        cfg = _w.load_config()
        host = host or os.environ.get("WASABI_HOST") or cfg.get("host")
        port = int(port or os.environ.get("WASABI_PORT") or
                   cfg.get("port", 1234))
        if timeout is not None:          # for this whole process
            _w.SILENCE = max(1.0, float(timeout))
        if not host:
            class _A:
                pass
            a = _A()
            a.host, a.port = None, port
            host, port = _w.resolve_target(a)
        # key@ADDRESS:PORT in the config, then WASABI_KEY, then key
        key = _w.key_for(host, port, key)
        self.native = native
        self._c = _w.Conn(host, port, key, tls=tls)

    # --- the connection -----------------------------------------------

    @property
    def host(self):
        return self._c.host

    @property
    def port(self):
        return self._c.port

    @property
    def banner(self):
        """The daemon's version, e.g. 'wasabid 0.10b4'."""
        return self._c.banner

    @property
    def caps(self):
        """What the daemon can do (a set of names, see PROTOCOL.md)."""
        return set(self._c.caps)

    @property
    def encrypted(self):
        return bool(self._c.tls)

    def close(self):
        self._c.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def ping(self):
        """Round trip in milliseconds."""
        import time
        t0 = time.time()
        self._c.send(_w.PING)
        tag, _ = self._c.recv()
        if tag != _w.PONG:
            raise WasabiError("no PONG")
        return (time.time() - t0) * 1000

    def info(self):
        """The daemon's INFO text."""
        out = []
        self._c.send(_w.INFO)
        _w.drain_to_end(self._c, out.append)
        return b"".join(out).decode("latin-1")

    # --- commands --------------------------------------------------------

    def run(self, command, max_time=None, merge=False):
        """Run an AmigaDOS command and wait for it: RunResult(rc,
        stdout, stderr). merge=True puts both in stdout, in order.
        max_time: Ctrl-C it after that many seconds (TimeLimit)."""
        out, err = [], []
        rc = _w.do_run(self._c, command, merge=merge, sink=out.append,
                       err_sink=out.append if merge else err.append,
                       max_time=max_time)
        return RunResult(rc, "".join(out), "".join(err))

    def start(self, command):
        """Start a command and do not wait for it (run --detach)."""
        _w.do_run(self._c, command, detach=True, sink=lambda s: None)

    def rexx(self, program, max_time=None):
        """Run an ARexx program (its text) with RX: RunResult(rc, what
        it SAYs, '')."""
        out = []
        rc = _w.rexx_program(self._c, program, sink=out.append,
                             max_time=max_time)
        return RunResult(rc, "".join(out), "")

    def arexx(self, port, command, timeout=30):
        """Send COMMAND to a program's ARexx port: RexxResult(rc, rc2,
        result). For ARexx itself (REXX, AREXX) use rexx()."""
        return RexxResult(*_w.arexx(self._c, port, command, timeout))

    def arexx_ports(self):
        return _w.rexx_ports(self._c)

    # --- files -------------------------------------------------------

    def ls(self, path=""):
        """[Entry(name, dir, size, protection, date)], drawers first."""
        return [Entry(name, kind == "d", None if kind == "d" else size,
                      _w.protstr(bits), when)
                for kind, size, bits, when, name in
                _w.ls_entries(self._c, path)]

    def put(self, local, remote):
        """Upload a file from this computer."""
        _w.do_put(self._c, local, remote, quiet=True)

    def write(self, remote, data):
        """Write bytes (or text, as Latin-1) to a file on the Amiga."""
        if isinstance(data, str):
            data = data.encode("latin-1")
        _w.put_bytes(self._c, data, remote)

    def read(self, remote):
        """A file's bytes; WasabiError when it is not there."""
        out = []
        self._c.send(_w.GET, _w.pack_str(remote))
        _w.drain_to_end(self._c, out.append)
        return b"".join(out)

    def get(self, remote, local):
        with open(local, "wb") as fh:
            fh.write(self.read(remote))

    def delete(self, path):
        self._c.send(_w.DEL, _w.pack_str(path))
        self._c.expect_ok()

    def mkdir(self, path):
        self._c.send(_w.MKDIR, _w.pack_str(path))
        self._c.expect_ok()

    # --- keyboard ----------------------------------------------------

    def key_type(self, text):
        """Type text through the Amiga's own keymap ('\\n' is Return)."""
        _w.send_text(self._c, text)

    def key_press(self, *combos):
        """Press keys by name: key_press('ramiga+w'), key_press('esc'),
        key_press('0x45'). Several are pressed one after another."""
        events = []
        for c in combos:
            events += _w.key_combo_events(c)
        _w.send_keys(self._c, events)

    # --- mouse -------------------------------------------------------

    def _y(self, y):
        if y is None or not self.native:
            return y
        return y * self._front_scale()

    def _front_scale(self):
        screens, _ = _w.list_windows(self._c)
        return screens[0]["row_scale"] if screens else 1

    def mouse_move(self, x, y):
        _w.send_mouse(self._c, 0, x, self._y(y))

    def click(self, x=None, y=None, button="left", double=False):
        """Click at x, y (or where the pointer is)."""
        _w.send_mouse(self._c, 1, x, self._y(y), _w.BUTTONS[button],
                      2 if double else 1)

    def mouse_down(self, x=None, y=None, button="left"):
        _w.send_mouse(self._c, 2, x, self._y(y), _w.BUTTONS[button])

    def mouse_up(self, x=None, y=None, button="left"):
        _w.send_mouse(self._c, 3, x, self._y(y), _w.BUTTONS[button])

    def wheel(self, direction, count=1):
        """direction 'up' or 'down'."""
        _w.send_keys(self._c, _w.key_combo_events("wheel" + direction)
                     * count)

    def pointer(self):
        """(x, y) of the pointer on the front screen."""
        screens = self.windows()
        if not screens or "pointer" not in screens[0]:
            raise WasabiError("this daemon (%s) does not say where the "
                              "pointer is" % self.banner)
        return tuple(screens[0]["pointer"])

    # --- timed batches -------------------------------------------------

    def input(self, steps, gap=1):
        """Play key and mouse events with fixed gaps, timed by the Amiga
        (ticks of 1/50 s): one request, the same timing every time.

        steps: 'key ramiga+w', 'type hello', 'move X Y', 'click [X Y]',
        'double [X Y]', 'down [X Y]', 'up [X Y]' (add 'right' or
        'middle' for that button), 'wait N' (N more ticks after the
        event before). Up to 1024 events and 5 s of waits per call."""
        records = _w.input_events(steps, gap)
        k = 1
        if self.native and any(r[0] == 2 and r[6] != _w.NOPOS
                               for r in records):
            k = self._front_scale()
        _w.send_inputs(self._c, records, k)

    # --- the screen ----------------------------------------------------

    def grab(self, window=None, region=None, screen="", raw=None):
        """The front screen (or a named screen) as an Image; window=
        a title to crop to, region=(x, y, w, h). raw: the screen's own
        rows (default: the native setting)."""
        raw = self.native if raw is None else raw
        box = None
        if window is not None:
            sc, win = _w.find_window(_w.list_windows(self._c)[0], window)
            if not win:
                raise WasabiError("no window with '%s' in its title"
                                  % window)
            k = sc["row_scale"] if raw else 1
            box = (win["left"], win["top"] // k, win["left"] + win["width"],
                   (win["top"] + win["height"]) // k)
        elif region is not None:
            x, y, w, h = region
            box = (x, y, x + w, y + h)
        w, _, h, pixels, _, _ = _w.grab_pixels(self._c, screen, raw)
        if box:
            x0, y0 = max(0, box[0]), max(0, box[1])
            x1, y1 = min(w, box[2]), min(h, box[3])
            if x1 <= x0 or y1 <= y0:
                raise WasabiError("that box is off the screen")
            pixels = _w.crop_rgb(pixels, w, (x0, y0, x1, y1))
            w, h = x1 - x0, y1 - y0
        return Image(w, h, pixels)

    def sum(self, region=None, window=None):
        """A checksum of the screen, a window or region=(x, y, w, h),
        computed on the Amiga: (hex string, pixels). Compare two to see
        whether it changed without moving a picture."""
        box = None
        if window is not None:
            box = _w.window_box(self._c, window)
        elif region is not None:
            x, y, w, h = region
            box = (x, y, x + w, y + h)
        k = 1
        if box and not (self.native and region is not None):
            k = self._front_scale()
        total, n = _w.region_sum(self._c, box, k)
        return "%08x" % total, n

    def windows(self):
        """Screens, front first, each a dict with its 'windows', front
        first (docs/api.md has the fields)."""
        screens, _ = _w.list_windows(self._c)
        return _w.native_out(screens, self.native)

    def screen_to_front(self, title):
        """Bring the screen with that title to the front."""
        import struct
        self._c.send(_w.SCREEN, struct.pack(">I", 0) + _w.pack_str(title))
        _w.drain_to_end(self._c, lambda b: None)

    # --- the console as text (Zenix, caps 'text') ----------------------

    def text(self, console=0, cells=False):
        """The console's text as a ConsoleText; console 0 is the one on
        the display, 1-10 a given one. cells=True adds each cell's style
        and colour."""
        info, _ = _w.console_text(self._c, console, cells=cells)
        return _console_text(info)

    def wait_text(self, pattern, timeout=30, console=0):
        """Wait until a line of the console matches pattern (a POSIX
        extended regular expression); the Amiga does the watching.
        Returns the ConsoleText; TimeLimit when timeout seconds pass."""
        info, met = _w.console_text(self._c, console, pattern=pattern,
                                    timeout=timeout)
        if not met:
            raise TimeLimit("text matching %r did not appear within %g s"
                            % (pattern, timeout))
        return _console_text(info)

    # --- gadgets and menus ---------------------------------------------

    def ui(self, window=None, all_screens=False):
        """Screens with their windows, each window with 'gadgets' and
        'menus'. window: only windows whose title contains it (then every
        screen is searched) - returns a list of windows."""
        screens = _w.read_ui(self._c, every_screen=all_screens or
                             window is not None)
        _w.native_out(screens, self.native)
        if window is None:
            return screens
        return [w for _, w in _w.ui_windows(screens, window)]

    def ui_click(self, name=None, window=None, kind=None, nth=None, id=None,
                 double=False):
        """Click a gadget by label, by kind, or by GadgetID (id=)."""
        return _w.ui_click(self._c, name, window, kind, nth, double,
                           gid=id)[1]

    def ui_set(self, name, text, window=None, kind=None, nth=None, id=None,
               enter=True):
        """Fill a text field (clear, type, Return unless enter=False)."""
        return _w.ui_set(self._c, name, text, window, kind, nth, id,
                         enter)[1]

    def ui_menu(self, path, window=None):
        """Pick a menu item: ui_menu('Project/Quit')."""
        return _w.ui_menu(self._c, path, window)[1]

    def ui_front(self, title):
        """Bring a window (and its screen) to the front and activate it."""
        pairs = _w.ui_windows(_w.read_ui(self._c, every_screen=True), title)
        if not pairs:
            raise WasabiError("no window titled %r" % title)
        _w.ui_front(self._c, pairs[0][1])

    # --- the clipboard ---------------------------------------------------

    def clip(self):
        return _w.clip_get(self._c)

    def set_clip(self, text):
        _w.clip_set(self._c, text)


def _console_text(info):
    return ConsoleText(info["lines"], info["rows"], info["cols"],
                       tuple(info["cursor"]), info["console"], info["shown"],
                       info["changes"], info.get("styles"),
                       info.get("colours"))
