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
VERSION = "0.1"
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
        return text(ok("put", local, a["path"]))


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

TOOLS = [
    ("amiga_info", t_info, "Kickstart, CPU, free memory, volumes and the "
     "daemon's version.", S()),
    ("amiga_run", t_run, "Run an AmigaDOS command and return its output and "
     "return code (0 ok, 5 warn, 10 error, 20 failure).",
     S(command=STR("the command line, e.g. 'list SYS:'", True),
       max_time=NUM("stop it after this many seconds"))),
    ("amiga_ls", t_ls, "List a drawer.", S(path=STR("e.g. 'SYS:C'"))),
    ("amiga_read_file", t_read_file, "Read a file. Text comes back as text "
     "(Latin-1), anything else as base64.", S(path=STR("Amiga path", True))),
    ("amiga_write_file", t_write_file, "Write a file. Give text, or base64 "
     "for binary data. System places (SYS:, C:, S:, LIBS: ...) are refused.",
     S(path=STR("Amiga path", True), text=STR("the contents as text"),
       base64=STR("the contents as base64"))),
    ("amiga_screenshot", t_screenshot, "The front screen, one window, or a "
     "region of the screen, as a PNG. Coordinates in it are the ones the "
     "mouse tools use.",
     {"type": "object", "properties": {
         "window": {"type": "string",
                    "description": "only this window (part of its title)"},
         "region": REGION}}),
    ("amiga_screens", t_screens, "List the open screens, front first.", S()),
    ("amiga_pointer", t_pointer, "Where the mouse pointer is on the front "
     "screen (x y, in screenshot coordinates). No picture is sent.", S()),
    ("amiga_drag", t_drag, "Press a button at x1,y1, move (through 'via' "
     "points if given) to x2,y2, release: drag and drop, sliders, and "
     "Amiga menus with the right button.",
     {"type": "object", "required": ["x1", "y1", "x2", "y2"], "properties": {
         "x1": {"type": "number"}, "y1": {"type": "number"},
         "x2": {"type": "number"}, "y2": {"type": "number"},
         "via": {"type": "array", "items": {"type": "array",
                                            "items": {"type": "number"}},
                 "description": "points to pass through, [[x,y],...]"},
         "button": {"type": "string", "description": "left (default), "
                    "right (menus) or middle"}}}),
    ("amiga_wait", t_wait, "Wait for something instead of sleeping: a "
     "window or task to appear (or go, with gone), a file, the screen to "
     "change or to settle (still).",
     S(what=STR("window, task, file, change or still"),
       name=STR("the window title, task name or file path; for change "
                "and still, watch only this window"),
       region=dict(REGION, description="change/still: watch only this "
                   "rectangle"),
       gone=BOOL("wait for it to go instead"),
       timeout=NUM("seconds (default 30)"))),
    ("amiga_region_changed", t_region_changed, "Has the screen, a window "
     "or a rectangle changed? The Amiga works out a checksum and sends only "
     "that - no picture. Call once to get a checksum, then again with it "
     "as 'since'.",
     S(window=STR("only this window (part of its title)"),
       region=dict(REGION), since=STR("the checksum from an earlier call"))),
    ("amiga_windows", t_windows, "Every screen and window, front first, with "
     "positions.", S()),
    ("amiga_ui_list", t_ui_list, "The gadgets (buttons, text fields, "
     "checkboxes, ...) and menu items of the windows on the front screen, "
     "by name, with positions and states: Intuition, GadTools, ReAction and "
     "MUI 4/5 windows. Custom-drawn content needs a screenshot and the "
     "mouse.",
     S(window=STR("only windows whose title contains this"),
       all_screens=BOOL("every screen, not only the front one"))),
    ("amiga_ui_click", t_ui_click, "Click a gadget by its label with the "
     "real mouse (close, depth, zoom, size also name the window's border "
     "gadgets). Brings the window to the front if needed.",
     S(name=STR("the gadget's label"), window=STR("part of the window title"),
       kind=STR("button, string, toggle, custom, ..."),
       nth=NUM("which of several matches, from 1"),
       double=BOOL("double-click"))),
    ("amiga_ui_set", t_ui_set, "Fill a text field: click it, clear it, type "
     "the text, press Return.",
     S(name=STR("the field's label"), text=STR("what to type", True),
       window=STR("part of the window title"), kind=STR("e.g. string"),
       nth=NUM("which of several matches"),
       no_enter=BOOL("do not press Return"))),
    ("amiga_ui_menu", t_ui_menu, "Pick a menu item by name, e.g. "
     "'Project/Save As' or just 'Quit'. Works without moving the mouse.",
     S(path=STR("menu path", True), window=STR("part of the window title; "
                                               "default the active window"))),
    ("amiga_mouse", t_mouse, "Move the pointer, click, or press/release a "
     "button (down/up, for drags) at screenshot coordinates.",
     S(action=STR("move, click, doubleclick, down or up"), x=NUM("x"),
       y=NUM("y"), button=STR("left (default), right or middle"))),
    ("amiga_key_type", t_key_type, "Type text through the Amiga's own "
     "keymap, into the active window.",
     S(text=STR("the text", True), enter=BOOL("press Return after it"))),
    ("amiga_key_press", t_key_press, "Press keys by name: return, esc, tab, "
     "f1-f12, up, home, end, pageup, pagedown, insert, kp0-kp9, kpenter, "
     "ramiga+q, ctrl+c ... or a raw key code like 0x45.",
     {"type": "object", "required": ["keys"], "properties": {
         "keys": {"type": "array", "items": {"type": "string"},
                  "description": "one or more key names"}}}),
    ("amiga_arexx", t_arexx, "Send a command to a program's ARexx port and "
     "get its RESULT. amiga_arexx_ports lists the ports.",
     S(port=STR("port name, case sensitive (e.g. WORKBENCH)", True),
       command=STR("the command", True),
       timeout=NUM("seconds to wait (default 30)"))),
    ("amiga_arexx_ports", t_arexx_ports, "List the public message ports, "
     "which include every ARexx port.", S()),
    ("amiga_slots", t_slots, "What amiga_run commands are running now, "
     "in which slot and for how long (four slots).", S()),
    ("amiga_break", t_break, "Stop a running command by its slot from "
     "amiga_slots: Ctrl-C, and if it will not stop in three seconds, let "
     "go of the slot.",
     S(slot=NUM("slot number", True),
       ctrl_c_only=BOOL("only send Ctrl-C, keep the slot"))),
    ("amiga_ps", t_ps, "List tasks and processes.",
     S(pattern=STR("AmigaDOS wildcard"))),
    ("amiga_kill", t_kill, "Stop a task: Ctrl-C, or with force RemTask.",
     S(target=STR("task name or 0xADDRESS from amiga_ps", True),
       force=BOOL("RemTask - last resort"))),
    ("amiga_health", t_health, "Temperatures, clocks, CPU load and memory "
     "(on a PiStorm), and the last guru.", S()),
]
BY_NAME = {n: (fn, desc, schema) for n, fn, desc, schema in TOOLS}


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
            {"name": n, "description": d, "inputSchema": s}
            for n, _, d, s in TOOLS]})
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
