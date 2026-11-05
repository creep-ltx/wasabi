#!/usr/bin/env python3
"""wasabi_mcp - Wasabi as an MCP server, so any MCP client (Claude Code,
Claude Desktop, Codex, Gemini CLI, Cursor, ...) can drive the Amiga.

A stdio JSON-RPC 2.0 server, standard library only. Every tool runs the
`wasabi` command next to this file, so discovery, the key in
~/.config/wasabi/config, WASABI_HOST/WASABI_PORT and the system-place
protection all work exactly as they do on the command line - this file
adds no second way of talking to the daemon.

  claude mcp add wasabi -- python3 /path/to/wasabi/wasabi_mcp.py

Claude Code sessions on this PC do not need it: they use the `wasabi`
skill. It is for other programs and other AI tools.
"""

import base64
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WASABI = os.path.join(HERE, "wasabi")
VERSION = "0.2"
PROTOCOLS = ("2025-06-18", "2025-03-26", "2024-11-05")


class ToolError(Exception):
    pass


def wasabi(*args, timeout=120, data=None):
    """Run the wasabi command; (exit code, stdout, stderr)."""
    try:
        # The one-file build is its own `wasabi`; otherwise the script
        # next to this file, run by the same Python.
        cmd = [sys.executable] if getattr(sys, "frozen", False) else \
            [sys.executable, WASABI]
        p = subprocess.run(cmd + [str(a) for a in args],
                           input=data, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise ToolError("wasabi %s took longer than %d s" % (args[0], timeout))
    return (p.returncode, p.stdout.decode("utf-8", "replace"),
            p.stderr.decode("utf-8", "replace"))


def ok(*args, timeout=120):
    """Run it and fail the tool on a non-zero exit; returns stdout."""
    rc, out, err = wasabi(*args, timeout=timeout)
    if rc != 0:
        raise ToolError((err or out).strip() or "wasabi %s failed (%d)"
                        % (args[0], rc))
    return out


def text(s):
    return [{"type": "text", "text": s if s.strip() else "(no output)"}]


# --- the tools ---------------------------------------------------------

def t_info(a):
    return text(ok("info"))


def t_run(a):
    args = ["run", "--merge"]
    if a.get("max_time"):
        args += ["--max-time", a["max_time"]]
    rc, out, err = wasabi(*args, a["command"],
                          timeout=float(a.get("max_time") or 600) + 30)
    body = out + (("\n" + err) if err.strip() else "")
    return text("%s\n[return code %d]" % (body.rstrip(), rc))


def t_ls(a):
    return text(ok("ls", a.get("path", "")))


def t_read_file(a):
    with tempfile.TemporaryDirectory() as d:
        local = os.path.join(d, "f")
        ok("get", a["path"], local)
        data = open(local, "rb").read()
    if b"\0" in data[:4096]:
        return text("binary file, %d bytes, base64:\n%s"
                    % (len(data), base64.b64encode(data).decode()))
    return text(data.decode("latin-1"))


def t_write_file(a):
    if "base64" in a:
        data = base64.b64decode(a["base64"])
    else:
        data = a.get("text", "").replace("\r\n", "\n").encode("latin-1",
                                                              "replace")
    with tempfile.TemporaryDirectory() as d:
        local = os.path.join(d, "f")
        open(local, "wb").write(data)
        ok("put", local, a["path"])
    return text("wrote %d bytes to %s" % (len(data), a["path"]))


def t_screenshot(a):
    with tempfile.TemporaryDirectory() as d:
        local = os.path.join(d, "s.png")
        args = ["grab", local]
        if a.get("window"):
            args += ["--window", a["window"]]
        if a.get("region"):
            r = a["region"]
            args += ["--region", "%d,%d,%d,%d" % (r["x"], r["y"], r["width"],
                                                  r["height"])]
        info = ok(*args)
        png = open(local, "rb").read()
    return [{"type": "image", "mimeType": "image/png",
             "data": base64.b64encode(png).decode()},
            {"type": "text", "text": info.strip()}]


def t_windows(a):
    return text(ok("windows"))


def t_ui_list(a):
    args = ["ui", "list"]
    if a.get("window"):
        args += ["--window", a["window"]]
    if a.get("all_screens"):
        args.append("--all-screens")
    return text(ok(*args))


def _where(a, args):
    if a.get("window"):
        args += ["--window", a["window"]]
    if a.get("kind"):
        args += ["--kind", a["kind"]]
    if a.get("nth"):
        args += ["--nth", a["nth"]]
    if a.get("id") is not None:
        args += ["--id", int(a["id"])]
    return args


def t_ui_click(a):
    args = _where(a, ["ui", "click", a.get("name", "")])
    if a.get("double"):
        args.append("--double")
    return text(ok(*args))


def t_ui_set(a):
    args = _where(a, ["ui", "set", a.get("name", ""), a["text"]])
    if a.get("no_enter"):
        args.append("--no-enter")
    return text(ok(*args))


def t_ui_menu(a):
    args = ["ui", "menu", a["path"]]
    if a.get("window"):
        args += ["--window", a["window"]]
    return text(ok(*args))


def t_mouse(a):
    action = a.get("action", "click")
    args = ["mouse", action]
    if a.get("x") is not None and a.get("y") is not None:
        args += [a["x"], a["y"]]
    if a.get("button") == "right":
        args.append("--right")
    elif a.get("button") == "middle":
        args.append("--middle")
    ok(*args)
    return text("done")


def t_pointer(a):
    return text(ok("mouse", "where"))


def t_drag(a):
    btn = ["--right"] if a.get("button") == "right" else \
        ["--middle"] if a.get("button") == "middle" else []
    ok("mouse", "move", a["x1"], a["y1"])
    ok("mouse", "down", *btn)
    try:
        for x, y in a.get("via", []):
            ok("mouse", "move", x, y, *btn)
        ok("mouse", "move", a["x2"], a["y2"], *btn)
    finally:
        ok("mouse", "up", *btn)          # never leave a button held
    return text("dragged")


def t_screens(a):
    return text(ok("screen", "--list"))


def t_wait(a):
    what = a.get("what", "window")
    args = ["wait", what]
    if what in ("window", "task", "file"):
        if not a.get("name"):
            raise ToolError("wait %s needs a name" % what)
        args.append(a["name"])
    elif a.get("name"):                  # change/still: one window
        args.append(a["name"])
    if a.get("region") and what in ("change", "still"):
        r = a["region"]
        args += ["--region", "%d,%d,%d,%d" % (r["x"], r["y"], r["width"],
                                              r["height"])]
    if a.get("gone"):
        args.append("--gone")
    args += ["--timeout", a.get("timeout", 30)]
    return text(ok(*args, timeout=float(a.get("timeout", 30)) + 30))


def t_key_type(a):
    args = ["key", "type", a["text"]]
    if a.get("enter"):
        args.append("--enter")
    ok(*args)
    return text("typed")


def t_key_press(a):
    ok("key", "press", *a["keys"])
    return text("pressed")


def t_arexx(a):
    rc, out, err = wasabi("--json", "arexx", a["port"], a["command"],
                          "--timeout", a.get("timeout", 30),
                          timeout=float(a.get("timeout", 30)) + 30)
    try:
        r = json.loads(out)
    except ValueError:
        raise ToolError((err or out).strip() or "arexx failed")
    if r["rc"] == 0:
        return text(r["result"] or "(ok, no result)")
    return text("rc %d (secondary %d)%s" % (r["rc"], r["rc2"],
                                            ("\n" + r["result"]) if
                                            r["result"] else ""))


def t_rexx(a):
    args = ["rexx"]
    if a.get("max_time"):
        args += ["--max-time", a["max_time"]]
    rc, out, err = wasabi(*args, "--", a["program"],
                          timeout=float(a.get("max_time") or 600) + 30)
    body = out + (("\n" + err) if err.strip() else "")
    return text("%s\n[rc %d]" % (body.rstrip(), rc))


def t_input(a):
    ok("input", "--gap", int(a.get("gap", 1)), "--", *a["steps"], timeout=30)
    return text("played")


def t_arexx_ports(a):
    return text(ok("arexx", "--ports"))


def t_ps(a):
    return text(ok("ps", a.get("pattern", "")))


def t_kill(a):
    args = ["kill", a["target"]]
    if a.get("force"):
        args.append("--force")
    return text(ok(*args))


def region_args(a):
    """--window / --region from a tool's arguments."""
    args = []
    if a.get("window"):
        args += ["--window", a["window"]]
    if a.get("region"):
        r = a["region"]
        args += ["--region", "%d,%d,%d,%d" % (r["x"], r["y"], r["width"],
                                              r["height"])]
    return args


def t_region_changed(a):
    args = ["--json", "sum"] + region_args(a)
    if a.get("since"):
        args += ["--since", a["since"]]
    rc, out, err = wasabi(*args)
    try:
        r = json.loads(out)
    except ValueError:
        raise ToolError((err or out).strip() or "sum failed")
    if "changed" in r:
        head = "changed" if r["changed"] else "unchanged"
    else:
        head = "first look"
    return text("%s - checksum %s (%d pixels); pass it as 'since' next time"
                % (head, r["sum"], r["pixels"]))


def t_slots(a):
    rc, out, err = wasabi("--json", "slots")
    if rc != 0:
        raise ToolError((err or out).strip() or "slots failed")
    rows = json.loads(out or "[]")
    if not rows:
        return text("all run slots are free")
    return text("\n".join(
        "slot %d: %s for %d s%s - %s" % (r["slot"], r["state"], r["seconds"],
                                         ", output read" if r["attached"]
                                         else "", r["command"])
        for r in rows))


def t_break(a):
    args = ["free", int(a["slot"])]
    if a.get("ctrl_c_only"):
        args.append("--ctrl-c-only")
    rc, out, err = wasabi(*args)
    if rc != 0:
        raise ToolError((err or out).strip() or "free failed")
    return text(err.strip() or out.strip())


def t_health(a):
    return text(ok("health"))


def t_text(a):
    args = ["text"]
    if a.get("console"):
        args += ["--console", int(a["console"])]
    return text(ok(*args))


def t_wait_text(a):
    args = ["wait", "text", a["pattern"], "--timeout", a.get("timeout", 30)]
    if a.get("console"):
        args += ["--console", int(a["console"])]
    return text(ok(*args, timeout=float(a.get("timeout", 30)) + 30))


REGION = {"type": "object", "description": "only this rectangle",
          "properties": {"x": {"type": "number"},
                         "y": {"type": "number"},
                         "width": {"type": "number"},
                         "height": {"type": "number"}},
          "required": ["x", "y", "width", "height"]}

S = lambda **p: {"type": "object", "properties": p,
                 "required": [k for k, v in p.items() if v.pop("req", False)]}
STR = lambda d, req=False: {"type": "string", "description": d, "req": req}
NUM = lambda d, req=False: {"type": "number", "description": d, "req": req}
BOOL = lambda d: {"type": "boolean", "description": d}

# Each tool: name, function, title, what it does to the Amiga, description,
# input schema. The second-to-last becomes the MCP annotations: LOOK only
# reads, ACT changes something that can be undone or done again, RISK can
# lose work (a program's unsaved data, a file, a task).
LOOK = {"readOnlyHint": True, "destructiveHint": False,
        "idempotentHint": True, "openWorldHint": False}
ACT = {"readOnlyHint": False, "destructiveHint": False,
       "idempotentHint": False, "openWorldHint": False}
RISK = {"readOnlyHint": False, "destructiveHint": True,
        "idempotentHint": False, "openWorldHint": False}

TOOLS = [
    ("amiga_info", t_info, "System information", LOOK,
     "Describe the Amiga: Kickstart and Workbench versions, CPU, free chip "
     "and fast memory, every mounted volume with its size and free space, "
     "and the daemon's version and the commands it supports (the 'can:' "
     "line). Call it first to learn which machine you are on and which "
     "volume names exist. Read-only. For temperatures, CPU load and the "
     "last crash use amiga_health instead.", S()),
    ("amiga_run", t_run, "Run an AmigaDOS command", RISK,
     "Run one AmigaDOS command line (as typed in a Shell) and wait for it "
     "to finish. Returns everything it printed, stdout and stderr merged, "
     "then a line '[return code N]': 0 ok, 5 warning, 10 error, 20 "
     "failure. The command can do anything a Shell can, including delete "
     "files. For a program that opens a window and keeps running, start "
     "it with 'Run >NIL: Program' so this returns at once, then use "
     "amiga_wait for its window. Stuck commands: amiga_slots and "
     "amiga_break. Prefer amiga_ls and amiga_read_file for looking at "
     "files.",
     S(command=STR("the command line, e.g. 'List SYS:C' or 'Version "
                   "exec.library'", True),
       max_time=NUM("stop it (Ctrl-C) after this many seconds; default: "
                    "wait up to 600 s"))),
    ("amiga_ls", t_ls, "List a directory", LOOK,
     "List the files and subdirectories ('drawers') in one Amiga "
     "directory, drawers first then by name. One line per entry: 'd' for "
     "a drawer or '-' for a file, the size in bytes ('(dir)' for "
     "drawers), the protection bits as List shows them (hsparwed), the "
     "date and time, the name. Amiga paths start with a volume or assign "
     "and a colon, e.g. 'SYS:', 'Work:Projects', 'RAM:'; '/' is the "
     "parent, not the root. With no path it lists the mounted volumes "
     "(on Zenix, '/'). Not recursive. Read-only.",
     S(path=STR("the directory, e.g. 'SYS:C' or 'Work:'; empty lists the "
                "volumes"))),
    ("amiga_read_file", t_read_file, "Read a file", LOOK,
     "Copy one file from the Amiga and return its contents. Text comes "
     "back as text (decoded as Latin-1, the Amiga's character set); a "
     "file with a zero byte in its first 4 KB is treated as binary and "
     "comes back as 'binary file, N bytes, base64:' and the data. Fails "
     "if the path does not exist or is a directory (use amiga_ls). "
     "Read-only.",
     S(path=STR("the full Amiga path, e.g. 'S:Startup-Sequence'", True))),
    ("amiga_write_file", t_write_file, "Write a file", RISK,
     "Create or replace one file on the Amiga. Give the contents either "
     "as 'text' (encoded as Latin-1, line ends become LF) or as 'base64' "
     "for binary data; not both. An existing file of that name is "
     "replaced. The parent directory must exist. System places (SYS:, C:, "
     "S:, LIBS:, DEVS:, L:, ...) are refused so a mistake cannot break "
     "the boot. Returns a line with the bytes written.",
     S(path=STR("the full Amiga path, e.g. 'RAM:test.txt'", True),
       text=STR("the contents as text"),
       base64=STR("the contents as base64, for binary files"))),
    ("amiga_screenshot", t_screenshot, "Take a screenshot", LOOK,
     "Capture the front screen as a PNG image, or only one window or a "
     "rectangle of it (smaller and faster). Also returns a line with the "
     "size. Pixel coordinates in the picture are exactly the ones "
     "amiga_mouse, amiga_drag and amiga_input take. Use it to see the "
     "screen before clicking; to find buttons by name amiga_ui_list is "
     "cheaper, and to check only whether something changed use "
     "amiga_region_changed. Read-only.",
     {"type": "object", "properties": {
         "window": {"type": "string",
                    "description": "only this window (part of its title)"},
         "region": REGION}}),
    ("amiga_screens", t_screens, "List screens", LOOK,
     "List the open Intuition screens, front first: address, size, "
     "colour depth, title, and which one is in front. Only screens; for "
     "the windows on them use amiga_windows. Read-only.", S()),
    ("amiga_pointer", t_pointer, "Mouse pointer position", LOOK,
     "Return where the mouse pointer is on the front screen, as 'x y' in "
     "screenshot coordinates. No picture is sent. Read-only.", S()),
    ("amiga_drag", t_drag, "Drag with the mouse", ACT,
     "Press a mouse button at x1,y1, move through the 'via' points if "
     "given, to x2,y2, and release. For drag and drop (icons, files), "
     "sliders and scroll bars, resizing, and Amiga menus (right button: "
     "hold it over the screen's title bar, drag to the item). The button "
     "is always released, even on an error. Coordinates as in "
     "amiga_screenshot. For a timed gesture use amiga_input; to pick a "
     "menu item by name use amiga_ui_menu.",
     {"type": "object", "required": ["x1", "y1", "x2", "y2"], "properties": {
         "x1": {"type": "number", "description": "start x"},
         "y1": {"type": "number", "description": "start y"},
         "x2": {"type": "number", "description": "end x"},
         "y2": {"type": "number", "description": "end y"},
         "via": {"type": "array", "items": {"type": "array",
                                            "items": {"type": "number"}},
                 "description": "points to pass through, [[x,y],...]"},
         "button": {"type": "string", "enum": ["left", "right", "middle"],
                    "description": "left (default), right (menus) or "
                    "middle"}}}),
    ("amiga_wait", t_wait, "Wait for something", LOOK,
     "Block until something happens on the Amiga, instead of sleeping: a "
     "window or task appears (or goes, with gone=true), a file appears, "
     "the screen changes, or the screen settles (stops changing). The "
     "Amiga watches, so nothing crosses the network meanwhile. Returns a "
     "line saying what happened; fails with an error after 'timeout' "
     "seconds. Use it after starting a program or clicking something "
     "that takes time. Read-only.",
     S(what=STR("window (default), task, file, change or still"),
       name=STR("for window/task/file: the window title, task name or file "
                "path (required); for change/still: watch only this window"),
       region=dict(REGION, description="change/still: watch only this "
                   "rectangle"),
       gone=BOOL("wait for the window, task or file to go away instead"),
       timeout=NUM("seconds (default 30)"))),
    ("amiga_region_changed", t_region_changed, "Did the screen change?",
     LOOK,
     "Tell whether the screen, a window or a rectangle has changed, "
     "without sending a picture: the Amiga works out a checksum. Call "
     "once without 'since' (answers 'first look' and a checksum), then "
     "later with that checksum as 'since' (answers 'changed' or "
     "'unchanged' and the new checksum). Cheap polling when a screenshot "
     "would be too big; to block until a change use amiga_wait with "
     "what=change. Read-only.",
     S(window=STR("only this window (part of its title)"),
       region=dict(REGION), since=STR("the checksum from an earlier call"))),
    ("amiga_windows", t_windows, "List windows", LOOK,
     "List every screen and the windows on it, front first: title, "
     "position, size, the task that owns it, and flags such as [active] "
     "and [backdrop]. Use it to find a window's title for the 'window' "
     "argument of other tools. For the buttons inside windows use "
     "amiga_ui_list. Read-only.", S()),
    ("amiga_ui_list", t_ui_list, "List buttons and menus", LOOK,
     "List the gadgets (buttons, text fields, checkboxes, cycle gadgets, "
     "sliders, lists and their rows) and the menu items of the windows on "
     "the front screen. One line per gadget: kind, label, id, position "
     "and size, the current value, and flags like [disabled] or "
     "[selected]; menu items as 'Menu/Item' paths with their shortcut. "
     "Works for Intuition, GadTools, ReAction and MUI 4/5 windows; "
     "custom-drawn content (games, paint programs) does not show up, so "
     "use amiga_screenshot there. Use it before amiga_ui_click, "
     "amiga_ui_set and amiga_ui_menu to learn the exact names. Read-only.",
     S(window=STR("only windows whose title contains this"),
       all_screens=BOOL("every screen, not only the front one"))),
    ("amiga_ui_click", t_ui_click, "Click a button by name", ACT,
     "Click a gadget found by its label (as amiga_ui_list shows it) with "
     "the real mouse, bringing its window to the front first if needed. "
     "'close', 'depth', 'zoom' and 'size' name a window's border "
     "gadgets. If several gadgets match, narrow it with window, kind or "
     "nth, or use the gadget's id. Returns which gadget was clicked, in "
     "which window; fails if none matches. A click can make "
     "the program do anything that button does (Quit, Delete, ...).",
     S(name=STR("the gadget's label"), window=STR("part of the window title"),
       kind=STR("button, string, toggle, cycle, custom, ..."),
       nth=NUM("which of several matches, from 1"),
       id=NUM("the gadget's id from amiga_ui_list (GadgetID); stays right "
              "when the layout moves"),
       double=BOOL("double-click"))),
    ("amiga_ui_set", t_ui_set, "Fill in a text field", ACT,
     "Fill a text (string) gadget found by its label: click it, clear "
     "it, type the text through the Amiga's keymap, and press Return "
     "unless no_enter is set. Returns which field was set. Find the "
     "field's label or id with amiga_ui_list first. For typing into a "
     "Shell or editor use amiga_key_type.",
     S(name=STR("the field's label"), text=STR("what to type", True),
       window=STR("part of the window title"), kind=STR("e.g. string"),
       nth=NUM("which of several matches"),
       id=NUM("the field's id from amiga_ui_list"),
       no_enter=BOOL("do not press Return"))),
    ("amiga_ui_menu", t_ui_menu, "Pick a menu item", ACT,
     "Choose a menu item by name, e.g. 'Project/Save As' or just 'Quit' "
     "if the name is unique, in the active window (or the one named). "
     "Works without moving the mouse or opening the menu on screen. The "
     "program then does whatever that item does. amiga_ui_list shows "
     "the menu paths.",
     S(path=STR("menu path, e.g. 'Project/Save As'", True),
       window=STR("part of the window title; default the active window"))),
    ("amiga_mouse", t_mouse, "Use the mouse", ACT,
     "Move the pointer to x,y and/or click there with the real mouse: "
     "move, click, doubleclick, or press (down) and release (up) a "
     "button. Without x,y it acts where the pointer is. Coordinates as "
     "in amiga_screenshot. Prefer amiga_ui_click when the target is a "
     "named gadget; use this for icons and custom-drawn areas. For a "
     "whole drag use amiga_drag.",
     S(action=dict(STR("move, click (default), doubleclick, down or up"),
                   enum=["move", "click", "doubleclick", "down", "up"]),
       x=NUM("x on the front screen"), y=NUM("y on the front screen"),
       button=dict(STR("left (default), right or middle"),
                   enum=["left", "right", "middle"]))),
    ("amiga_key_type", t_key_type, "Type text", ACT,
     "Type text into the active window through the Amiga's own keymap, "
     "as if typed on its keyboard. Make the right window active first "
     "(amiga_ui_click or amiga_mouse). For special keys and shortcuts "
     "(Return, F1, Amiga+Q) use amiga_key_press; to fill a named text "
     "field use amiga_ui_set.",
     S(text=STR("the text", True), enter=BOOL("press Return after it"))),
    ("amiga_key_press", t_key_press, "Press keys", ACT,
     "Press keys by name, one after another, into the active window: "
     "return, esc, tab, del, backspace, space, f1-f10, up, down, left, "
     "right, help, kp0-kp9, kpenter; modifiers joined with '+': "
     "ramiga+q, lamiga+m, ctrl+c, shift+f1, alt+x; or a raw key code like "
     "0x45. Each entry is pressed and released. For ordinary text use "
     "amiga_key_type.",
     {"type": "object", "required": ["keys"], "properties": {
         "keys": {"type": "array", "items": {"type": "string"},
                  "description": "one or more key names, e.g. "
                  "[\"ramiga+w\"] or [\"down\", \"return\"]"}}}),
    ("amiga_input", t_input, "Play a gesture", ACT,
     "Play a sequence of mouse and keyboard events in one request, timed "
     "on the Amiga in ticks of 1/50 s, for gestures that must be exact: "
     "a fast drag, a key chord, a burst of typing. Steps: 'key ramiga+w', "
     "'type hello', 'move X Y', 'click [X Y]', 'double [X Y]', "
     "'down [X Y]', 'up [X Y]' (add 'right' or 'middle' for that "
     "button), 'wait N' (N more ticks). At most 5 s of waits in total. "
     "For single actions the separate mouse and key tools are simpler.",
     {"type": "object", "required": ["steps"], "properties": {
         "steps": {"type": "array", "items": {"type": "string"},
                   "description": "the steps, in order"},
         "gap": {"type": "integer",
                 "description": "ticks between events (default 1)"}}}),
    ("amiga_rexx", t_rexx, "Run an ARexx program", RISK,
     "Run an ARexx program given as text and wait for it to end. Returns "
     "what it SAYs, then '[rc N]'. ARexx can call any program's port and "
     "run AmigaDOS commands, so it can change anything. Needs RexxMast "
     "running. To send one command to one program's port use amiga_arexx.",
     S(program=STR("the ARexx program, e.g. 'say 1+1'", True),
       max_time=NUM("stop it after this many seconds; default: wait up to "
                    "600 s"))),
    ("amiga_arexx", t_arexx, "Send an ARexx command", RISK,
     "Send one command to a running program's ARexx port and return its "
     "RESULT string, or 'rc N (secondary M)' when the program reports an "
     "error. What the command does depends on the program (WORKBENCH "
     "takes e.g. 'getattr ...', 'window ...'); some commands save, quit "
     "or delete. Port names are case sensitive; amiga_arexx_ports lists "
     "them. For a whole ARexx script use amiga_rexx.",
     S(port=STR("port name, case sensitive (e.g. WORKBENCH)", True),
       command=STR("the command", True),
       timeout=NUM("seconds to wait for the answer (default 30)"))),
    ("amiga_arexx_ports", t_arexx_ports, "List ARexx ports", LOOK,
     "List the public message ports, one name per line. Every program "
     "that can be scripted with ARexx shows up here under its port name, "
     "for amiga_arexx. Read-only.", S()),
    ("amiga_slots", t_slots, "Running commands", LOOK,
     "List the amiga_run commands that are running now: the daemon has "
     "four run slots; each line gives the slot number, its state, how "
     "many seconds it has run, and the command. Answers 'all run slots "
     "are free' when none run. Use it when amiga_run timed out or seems "
     "stuck, then amiga_break. For all tasks on the machine use amiga_ps. "
     "Read-only.", S()),
    ("amiga_break", t_break, "Stop a running command", RISK,
     "Stop a command started by amiga_run, by its slot number from "
     "amiga_slots: sends Ctrl-C, and if it has not stopped within three "
     "seconds, frees the slot anyway (the program is then left running "
     "until it ends, and amiga_slots shows it as stuck). "
     "The program loses whatever it had not saved. To stop any other task "
     "use amiga_kill.",
     S(slot=NUM("slot number from amiga_slots", True),
       ctrl_c_only=BOOL("only send Ctrl-C and keep the slot"))),
    ("amiga_ps", t_ps, "List tasks", LOOK,
     "List the tasks and processes running on the Amiga, like a Unix "
     "'ps': a header line, then one line per task with its address, kind "
     "(task or proc), priority, state (run, ready, wait), stack size and "
     "free stack, CLI number, and name (with the command it runs). "
     "'pattern' keeps only names matching an AmigaDOS wildcard, e.g. "
     "'#?shell#?' (#? is the Amiga's *). The address or name goes to "
     "amiga_kill. Read-only.",
     S(pattern=STR("AmigaDOS wildcard on the task name, e.g. '#?CLI#?'"))),
    ("amiga_kill", t_kill, "Stop a task", RISK,
     "Stop a task or process by name or by address from amiga_ps. By "
     "default it sends Ctrl-C, which well-behaved programs obey and "
     "clean up after. force=true removes the task with RemTask: its "
     "memory, files and windows are left behind and the system may crash, "
     "so use it only when Ctrl-C has failed. Returns what was done.",
     S(target=STR("task name or 0xADDRESS from amiga_ps", True),
       force=BOOL("RemTask instead of Ctrl-C - last resort"))),
    ("amiga_health", t_health, "Health and last crash", LOOK,
     "Report the machine's health: on a PiStorm/Emu68 the Pi's "
     "temperature, voltages, clocks, under-voltage and throttling, the "
     "68k speed and JIT cache use; on every Amiga the free chip and fast "
     "memory with the largest blocks, and the last guru meditation "
     "(crash) with its code, if one happened since the Amiga booted. "
     "Use it when the machine seems slow or after a crash. Read-only.",
     S()),
    ("amiga_text", t_text, "Read the console (Zenix)", LOOK,
     "Zenix only (not AmigaOS): return the text on a Zenix console as "
     "lines, so the screen can be read without a picture. On AmigaOS "
     "it fails: use amiga_screenshot. Read-only.",
     S(console=NUM("console 1-10 (default: the one shown)"))),
    ("amiga_wait_text", t_wait_text, "Wait for console text (Zenix)", LOOK,
     "Zenix only (not AmigaOS): wait until a line of the console matches "
     "a pattern (an extended regular expression), e.g. 'login:'. The "
     "Amiga watches, so nothing crosses the network meanwhile. Answers "
     "when a line matches; fails after 'timeout' seconds. Read-only.",
     S(pattern=STR("the pattern", True),
       timeout=NUM("seconds (default 30)"),
       console=NUM("console 1-10 (default: the one shown)"))),
]
BY_NAME = {n: (fn, desc, schema) for n, fn, _, _, desc, schema in TOOLS}


# --- JSON-RPC over stdio -------------------------------------------------

def reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def handle(req):
    method, id_ = req.get("method"), req.get("id")
    params = req.get("params") or {}
    if method == "initialize":
        want = params.get("protocolVersion")
        return reply(id_, {
            "protocolVersion": want if want in PROTOCOLS else PROTOCOLS[0],
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "wasabi", "version": VERSION},
            "instructions": "Drives a real Amiga (or an emulator) through "
            "Wasabi. Look before acting: amiga_screenshot or amiga_ui_list "
            "first. Keys go to the active window."})
    if method == "ping":
        return reply(id_, {})
    if method == "tools/list":
        return reply(id_, {"tools": [
            {"name": n, "title": title, "description": d, "inputSchema": s,
             "annotations": dict(hints, title=title)}
            for n, _, title, hints, d, s in TOOLS]})
    if method == "tools/call":
        name = params.get("name")
        if name not in BY_NAME:
            return reply(id_, error={"code": -32602,
                                     "message": "unknown tool %r" % name})
        try:
            content = BY_NAME[name][0](params.get("arguments") or {})
            return reply(id_, {"content": content, "isError": False})
        except (ToolError, KeyError, ValueError) as e:
            what = ("missing argument %s" % e) if isinstance(e, KeyError) \
                else str(e)
            return reply(id_, {"content": text(what), "isError": True})
    if id_ is not None:                 # a request we do not know
        reply(id_, error={"code": -32601, "message": "unknown method"})
    # notifications (initialized, cancelled) need no answer


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--probe":
        rc, out, err = wasabi("ping")
        print((out or err).strip())
        return rc
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            reply(None, error={"code": -32700, "message": "parse error"})
            continue
        handle(req)
    return 0


if __name__ == "__main__":
    sys.exit(main())
