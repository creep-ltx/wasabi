# The Python API

[Back to the README](../README.md)

`wasabi_client.py` drives an Amiga from your own Python: test rigs,
scripts, tools. Its names, arguments and return values are a promise:
they change only with a new major `API_VERSION`. The `wasabi` command's
own functions are not - they change whenever the command does - so
import this module, not the script.

```python
import wasabi_client

with wasabi_client.connect() as amiga:
    print(amiga.banner)                          # wasabid 0.10
    r = amiga.run("Version FULL")
    print(r.rc, r.stdout)
    amiga.key_type("dir\n")
    amiga.input(["down 100 50", "move 180 50", "wait 5", "up"])
    amiga.ui_click("Save", window="Prefs")
    amiga.grab(window="Prefs").save("prefs.png")
```

**Setup:** put `wasabi_client.py` beside the `wasabi` script (both are
in every release; the module loads the script). Python 3.8 or later,
standard library only. `WASABI_SCRIPT=/path/to/wasabi` points it at a
script somewhere else.

**API version 1.1** (wasabi 0.11b3; 1.0 was wasabi 0.10). `wasabi_client.API_VERSION` says
which one you have. A minor version (1.1) only adds; a major one (2.0)
may change or remove something, and the change log says what.

## Connecting

`connect(host=None, port=None, key=None, tls=None, native=False,
timeout=None)` returns an `Amiga`. Use it in a `with` block, or call
`close()`.

| Argument | Meaning |
|---|---|
| `host`, `port`, `key` | as `--host`/`--port`/`--key`. Not given: `WASABI_HOST`/`WASABI_PORT`, then `~/.config/wasabi/config`, then discovery on the network; the key: the config's `key@ADDRESS:PORT` line for that machine, then `WASABI_KEY`, then the config's `key` - exactly as the command finds them |
| `tls` | `None`: as the command decides (encrypted when this computer has keys; never a silent downgrade). `True`: insist. `False`: never encrypt (`--plain`) |
| `native` | coordinates in the screen's own pixels instead of the picture's ([coordinates](screen-and-gui.md#coordinates)) - in and out |
| `timeout` | seconds of silence before `AmigaSilent` (default 15; for the whole process) |

One `Amiga` is one connection: one call at a time. For parallel work
(a `run` in one thread, clicks in another) open a second one.

Properties: `host`, `port`, `banner` (`'wasabid 0.10'`), `caps` (a set
of what the daemon can do, see [PROTOCOL.md](../PROTOCOL.md)),
`encrypted`.

## Errors

Every call raises one of these, all from `wasabi_client.WasabiError`:

| Exception | When |
|---|---|
| `WasabiError` | the command failed, or the Amiga said no (the message says why) |
| `TimeLimit` | `max_time` ran out (the command was stopped with Ctrl-C) |
| `AmigaSilent` | the Amiga stopped answering |

`OSError` (connection refused, reset) comes through as it is.

## Calls

### Commands

| Call | Returns |
|---|---|
| `ping()` | round trip in ms |
| `info()` | the daemon's info text |
| `run(command, max_time=None, merge=False)` | `RunResult(rc, stdout, stderr)`; `merge=True` puts both in `stdout`, in order |
| `start(command)` | starts it and returns at once (`run --detach`) |
| `rexx(program, max_time=None)` | runs an ARexx program (its text) with `RX`: `RunResult(rc, what it SAYs, '')` |
| `arexx(port, command, timeout=30)` | `RexxResult(rc, rc2, result)` from a program's ARexx port |
| `arexx_ports()` | the public port names |

### Files

| Call | Returns |
|---|---|
| `ls(path='')` | `[Entry(name, dir, size, protection, date)]`, drawers first; `''` lists the volumes |
| `read(remote)` | the file's bytes |
| `write(remote, data)` | writes bytes, or text as Latin-1 |
| `get(remote, local)`, `put(local, remote)` | copy a file down or up |
| `delete(path)`, `mkdir(path)` | |

The command's safety catch for system places (`--force`) is not in the
API: a script that writes to `C:` meant to.

### Keyboard and mouse

| Call | Does |
|---|---|
| `key_type(text)` | types through the Amiga's own keymap; `\n` is Return |
| `key_press(*combos)` | `key_press('ramiga+w')`, `key_press('esc', 'f1')`, `key_press('0x45')` - names as `wasabi key press` |
| `mouse_move(x, y)` | |
| `click(x=None, y=None, button='left', double=False)` | no x/y: where the pointer is; `button` `left`, `right`, `middle` |
| `mouse_down(x=None, y=None, button='left')`, `mouse_up(...)` | press and hold, release: drags and menus |
| `wheel('up' or 'down', count=1)` | |
| `pointer()` | `(x, y)` on the front screen |

### Timed input

`input(steps, gap=1)` sends a whole gesture in one request; the Amiga
plays it with `gap` ticks (1/50 s) between events, so the timing is
the same every time, whatever the network does. Steps:

```
key COMBO...                 keys by name: 'key ramiga+w'
type TEXT                    characters, through the keymap
move X Y
click|double|down|up [X Y]   add 'right' or 'middle' for that button
wait N                       N more ticks after the event before it
```

```python
amiga.input(["down 100 50", "move 140 50", "move 180 50", "wait 5", "up"])
amiga.input(["key lamiga+m"])                     # a chord
amiga.input(["type hello world"], gap=0)         # a burst
```

Up to 1024 events and 5 s of waits per call; it returns when the last
event is done. Needs wasabid 0.10 (`'inputs' in amiga.caps`).

### The screen

| Call | Returns |
|---|---|
| `grab(window=None, region=None, screen='', raw=None)` | an `Image`: the front screen, one window (by title), or `region=(x, y, w, h)`; `screen=` a screen by title |
| `sum(region=None, window=None)` | `(hex, pixels)`: a checksum worked out on the Amiga - compare two to see whether something changed, without a picture |
| `windows()` | screens, front first, each with its `windows` |
| `screen_to_front(title)` | |

`Image` has `width`, `height`, `rgb` (3 bytes a pixel, rows top down),
`pixel(x, y)` and `save(path)` (PNG).

A screen is a dict: `title`, `addr`, `left`, `top`, `width`, `height`,
`depth`, `row_scale`, `front`, `pointer` (`[x, y]`), `windows`. A window:
`title`, `addr`, `task`, `left`, `top`, `width`, `height`, `border`
(`[left, top, right, bottom]`), `active`, `backdrop`.

### The console as text (Zenix)

Zenix's daemon (caps `text`) gives its console as text, so a script
reads the screen without a picture; an AmigaOS wasabid has no such
thing (`WasabiError`).

| Call | Returns |
|---|---|
| `text(console=0, cells=False)` | a `ConsoleText`: console 0 is the one on the display, 1-10 a given one |
| `wait_text(pattern, timeout=30, console=0)` | waits until a line matches `pattern` (a POSIX extended regular expression) - the Amiga does the watching - and returns the `ConsoleText`; `TimeLimit` when `timeout` seconds pass |

`ConsoleText` has `lines` (trailing spaces gone), `rows`, `cols`,
`cursor` (`(row, column)`), `console`, `shown`, `changes` (a counter
raised on every draw), and with `cells=True` `styles` and `colours`:
each row's bytes as hex (colour: foreground in the low nibble,
background in the high one); else `None`.

### Gadgets and menus

| Call | Returns |
|---|---|
| `ui(window=None, all_screens=False)` | screens (or, with `window=`, the matching windows) with `gadgets` and `menus` |
| `ui_click(name=None, window=None, kind=None, nth=None, id=None, double=False)` | the gadget clicked |
| `ui_set(name, text, window=None, kind=None, nth=None, id=None, enter=True)` | fills a text field: clear, type, Return |
| `ui_menu(path, window=None)` | picks `'Project/Quit'` (or just `'Quit'`) |
| `ui_front(title)` | brings a window and its screen to the front |

A gadget: `kind`, `label`, `value`, `id`, `class`, `x`, `y`, `width`,
`height` (screen-relative, so its middle is where a click goes),
`disabled`, `selected`, `addr`. A menu item: `path`, `key`, `code`,
`disabled`, `checkable`, `checked`, `bar`.

**By id:** `id` is the gadget's `GadgetID` (GadTools, ReAction,
Intuition) or an MUI object's `MUIA_ObjectID` (wasabid 0.10+). It
stays the same when the window is laid out differently, where `nth`
does not: `ui_click(id=12, window="Prefs")`. Several gadgets with the
same id (border gadgets are usually 0) need `nth`.

### The clipboard

`clip()` returns its text; `set_clip(text)` sets it.

## Change log

- **1.1** (wasabi 0.11b3) - `text()` and `wait_text()` (Zenix's
  console); the key for a machine comes from its `key@ADDRESS:PORT`
  config line when one is there.
- **1.0** (wasabi 0.10) - the first stable API.
