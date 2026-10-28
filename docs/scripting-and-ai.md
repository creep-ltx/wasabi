# Scripts and AI assistants

[Back to the README](../README.md)

## Made for scripts, and for Claude

Wasabi is meant to be a tool a script — or Claude — can reach for
without a person watching. That needs four promises.

(Writing the script in Python? [The Python API](api.md) —
`import wasabi_client` — gives you the same things as calls, with a
documented API that does not change under you.)

**Nothing hangs.** Every reply has a deadline: if the Amiga takes a
connection and then says nothing for `--silence` seconds (default 15),
the command stops with *"the Amiga stopped answering"* and exit code
125. `run` is the hard case, because a command may rightly say nothing
for minutes (a compile, a `Wait`): after every 10 quiet seconds it pings
the daemon on a second connection, so a quiet program keeps running and
a frozen machine is reported. `run --max-time S` is the other limit,
the one *you* set: past it the command gets a Ctrl-C, ten seconds to
finish, and `run` ends with exit 124 either way, saying whether it
stopped. A command that ignores Ctrl-C keeps running on the Amiga —
the message says so, and how to remove it.

**Wait for things instead of guessing.** `wasabi wait` checks every half
second and returns the moment it happens (exit 0), or gives up after
`--timeout` (default 30 s, exit 124):

```
wasabi wait window "Prefs"            # a window whose title contains it
wasabi wait window "Prefs" --gone     # ...until it closes
wasabi wait task 'MyProg#?' --gone    # a task ends (AmigaDOS pattern)
wasabi wait file RAM:log --contains done
wasabi wait change                    # the front screen changes
wasabi wait still "Prefs"             # a window stops changing (1 s)
wasabi wait up --timeout 120          # the Amiga answers again
wasabi debug --until "ready" --for 20 # a program says something
```

`wasabi reboot --yes --wait` reboots and returns when the daemon answers
again.

**See and read, briefly.** `wasabi grab` grabs the front screen — or,
with `--window TITLE`, just that window — writes a PNG and prints only
its path on stdout (a new file in `$TMPDIR/wasabi-grab/` each time,
unless you name one), so a program that reads pictures can open it.
`--window` and `--diff` combine: "did my click change this window?"
`--json` on `ls`, `ps`, `screen`, `windows`, `ping`, `run`, `wait` and
`grab` prints one line of
JSON instead of a table. Errors always go to stderr as one line.

**Exit codes mean one thing each:**

| Code | Meaning |
|---|---|
| 0 | it worked (for `run`: the Amiga command's own code — 0, 5, 10, 20) |
| 1 | the command failed, or the Amiga said no |
| 2 | the Amiga could not be reached, or the command line was wrong |
| 124 | a time limit you set ran out (`--max-time`, `--timeout`, `--for` with `--until`) |
| 125 | the Amiga stopped answering |

**A safety catch.** Deleting or writing in a system place needs
`--force`: `SYS:` `C:` `L:` `LIBS:` `DEVS:` `S:` `FONTS:` `LOCALE:`
`ENVARC:`, plus any volume named in the config file's `protect =` line
(the boot volume's own name belongs there — `protect = AmigaOS` on the
A1200, since `SYS:` is just another name for it). `reboot` needs `--yes`,
as `quit` always has; `kill --force` (RemTask) was already explicit. A
`run` is not checked — `wasabi run "Delete C:#?"` does what it says.

## MCP: Wasabi for other AI tools

`wasabi mcp` (or `wasabi_mcp.py`, the same server) is an MCP server
(standard library only) that offers the
Amiga as tools — `amiga_run`, `amiga_read_file`, `amiga_write_file`,
`amiga_screenshot`, `amiga_ui_list`, `amiga_ui_click`, `amiga_ui_set`,
`amiga_ui_menu`, `amiga_mouse`, `amiga_drag`, `amiga_pointer`,
`amiga_screens`, `amiga_wait`, `amiga_key_type`, `amiga_key_press`,
`amiga_arexx`, `amiga_region_changed` (a checksum from the Amiga
instead of a screenshot), `amiga_slots` and `amiga_break` (list and stop
running commands), `amiga_text` and `amiga_wait_text` (Zenix's
console as text: read it, or wait for a line) and more - 30 in all
— to any MCP client: Claude Desktop, Codex CLI, Gemini CLI, Cursor,
Zed, LM Studio. Every tool runs the `wasabi` command, so discovery, the
key and the system-place protection are the same as on the command line.

```
claude mcp add --scope user wasabi -- wasabi mcp
claude mcp add --scope user wasabi -- python3 /path/to/wasabi/wasabi_mcp.py
```

The first works with any download (Linux, macOS, Windows) or the script
on your path; the second with a clone.

`--scope user` makes it available in every Claude Code session and
project; leave it out for the current project only.
`claude mcp remove --scope user wasabi` takes it away again.

For Claude Desktop and most others, the `mcpServers` block:

```json
{ "mcpServers": { "wasabi": {
    "command": "/path/to/wasabi",
    "args": ["mcp"] } } }
```

(`wasabi.exe` on Windows; with a clone, `"command": "python3"` and
`"args": ["/path/to/wasabi/wasabi_mcp.py"]`.)

`python3 wasabi_mcp.py --probe` checks the link without any AI. Claude
Code sessions on this PC do not need it - they use the `wasabi` skill.
