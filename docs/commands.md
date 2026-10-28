# Commands

[Back to the README](../README.md)

## Commands

```
wasabi discover              find Amigas on this network
wasabi ping                  round-trip time and daemon banner
wasabi info                  version, RAM, volumes, and what it can do
wasabi ls [PATH]             list a drawer; no path lists the volumes
wasabi put LOCAL REMOTE      upload a file (--force in a system place;
                             Zenix: --mode 755 sets its Unix mode)
wasabi get REMOTE LOCAL      download a file ('-' for stdout)
wasabi run "CMD" [--max-time S] [--detach]
                             execute, stream output, return its exit code;
                             Ctrl-C it if it runs past S seconds; --detach
                             starts it and returns at once (output to T:)
wasabi clip get | set [TEXT|-]
                             read or set the Amiga's clipboard
wasabi deploy L R [--run C] [--reboot | --restart] [--force]
wasabi del PATH [--force] / mkdir PATH
wasabi reboot --yes [--wait [S]] [--to amigaos|zenix] [--force]
                             reboot; --wait until it answers again (on
                             1234 or 1236); Zenix: --to picks what boots
                             next, --force skips its guard
wasabi restart [--force]     reload the daemon in place (a debug tool -
                             'update' is the verified path)
wasabi update LOCAL          replace the daemon itself, verifying first
wasabi quit --yes [--force]  stop the daemon (needs physical access after)
wasabi debug [--with-snoop] [--log F] [--for S] [--until TEXT]
                             live KPrintF stream
wasabi snoop [--task PAT] [--log F] [--for S] [--until TEXT]
                             live DOS call trace
wasabi ps [PATTERN]          list every task, with stack headroom;
                             AmigaDOS wildcards filter (Zenix: pids)
wasabi kill NAME|0xADDR|PID  Ctrl-C a task; --force for RemTask. NAME
                             may be the bare program name: Clock finds
                             one started as SYS:Utilities/Clock
wasabi speedtest [SIZE] [--target PATH]  latency and throughput both ways
wasabi grab [FILE] [--window TITLE | --region X,Y,W,H] [--diff BASE.png]
                             the front screen, or one window, as a PNG;
                             prints its path (no FILE: a new file each
                             time); --diff reports what changed, exits 1
wasabi screen [--cycle|--to-front T]    list screens, flip between them
wasabi desktop [--kiosk] [--no-browser]
                             the desktop app: overview, screen, files,
                             screenshots, in one window
wasabi view [--kiosk] [--no-browser]
                             the Amiga's screen in a window: your mouse
                             and keyboard drive it
wasabi health [--watch [S]]  the Pi's temperature, voltage and power
                             history, Emu68's CPU meter, free memory
wasabi windows [--raw]       every screen and window: place, size, task
wasabi wait window|task|file|change|still|up|text [NAME] [--gone]
            [--contains TEXT] [--region X,Y,W,H] [--timeout S]
                             wait for something instead of sleeping;
                             text REGEX [--console N]: a line of Zenix's
                             console matches (the Amiga watches)
wasabi text [--console N] [--json [--cells]]
                             Zenix: the console's text, no picture
wasabi sum [--window TITLE | --region X,Y,W,H] [--since SUM]
                             a checksum of the screen (or part of it),
                             worked out on the Amiga; --since exits 1
                             if nothing changed
wasabi mouse where           where the pointer is (x y)
wasabi mouse move|click|doubleclick|down|up [X Y] [--right|--middle]
                             move the pointer, click, drag, drive menus
wasabi key type TEXT [--enter]
                             type text, through the Amiga's own keymap
wasabi key press KEY...      press keys by name: return, esc, f1-f12, up,
                             home, end, pageup, pagedown, insert, kp0-kp9,
                             kpenter, ramiga+w, ctrl+c, or a code: 0x45
wasabi input STEP... [--gap TICKS] [--file F]
                             a batch of key and mouse events in one
                             request, played by the Amiga with fixed gaps
                             (ticks of 1/50 s): 'down 10 20' 'move 200 20'
                             'wait 5' 'up' 'key ramiga+w' 'type hello'
wasabi ui list|menus [--window T] [--all-screens]
                             gadgets (with their GadgetID) and menu items
wasabi ui click NAME [--window T] [--kind K] [--nth N] [--double]
                             click a gadget by its label (or close,
                             depth, zoom, size, drag)
wasabi ui click --id N       click the gadget with that GadgetID - stays
                             right when the layout moves
wasabi ui set NAME TEXT [--no-enter]   fill a text field (or --id N TEXT)
wasabi ui menu PATH [--window T]       pick 'Project/Save As' or 'Quit'
wasabi ui front TITLE        bring a window to the front, activate it
wasabi keys init|amiga NAME|drive DIR NAME|show|forget HOST
                             the encryption keys (see Security)
wasabi mcp                   the MCP server on stdin/stdout, for AI
                             assistants (see Scripts and AI)
wasabi arexx PORT COMMAND [--timeout S]
                             send an ARexx command, print RESULT; exit
                             code = its rc. No PORT: list the ports
wasabi rexx 'PROGRAM' | --file F [--max-time S]
                             run an ARexx program: what it SAYs comes
                             back, the exit code is its rc ('arexx REXX'
                             and 'arexx AREXX' do the same)
```

### `wasabi input` steps

```
key COMBO...                 keys by name, as 'key press': ramiga+w esc
type TEXT                    characters, through the Amiga's keymap
move X Y                     the pointer
click|double|down|up [X Y]   a button (left; add right or middle)
wait N                       N more ticks after the step before it
```

Each step is one argument (quote it), or one line of `--file` (`-` is
stdin; `#` starts a comment). `--gap` is the wait between every two
events (default 1 tick; 0 = back to back). The waits may add up to
5 s per batch. A drag that Intuition sees as a drag:

```
wasabi input "down 100 50" "move 140 50" "move 180 50" "wait 5" "up"
```

`--native` (or `coords = native` in the config, or
`WASABI_COORDS=native`) makes every coordinate the screen's own pixels;
see [coordinates](screen-and-gui.md#coordinates).

Every command also takes `--json` (where it lists something),
`--plain` (no encryption this time) and `--silence S` — see [Made for scripts](scripting-and-ai.md#made-for-scripts-and-for-claude).
`--host` overrides discovery, and `WASABI_HOST`/`WASABI_PORT`/`WASABI_KEY`/
`WASABI_SILENCE` override the config file (a `key@ADDRESS:PORT` line
there wins over `WASABI_KEY` for that machine; `--key` wins over all). The daemon takes `wasabid [port] [name ID] [allow
CIDR|any] [plain]`, and is LAN-only unless told otherwise; `plain` lets
unencrypted clients in even when it has TLS. `name` is what
discovery replies call the machine — two Amigas on one LAN stop being
interchangeable "amiga"s — and it overrides `ENV:HOSTNAME`, which is
the fallback (then plain `amiga`). It survives `restart` and
self-update, like the allow-list:

```
Run >NIL: C:wasabid name a1200
```

### Zenix

[Zenix](../PROTOCOL.md#zenixs-daemon) has a daemon of its own that
speaks Wasabi's protocol on **port 1236** (AmigaOS's wasabid keeps
1234), plain, with its own key. The commands work as they do on
AmigaOS where Zenix has the thing (`ping`, `info`, `ls`, `put`, `get`,
`run`, `ps`, `kill`, `debug`, `grab`, `key`, `health`, `speedtest`,
`reboot`, ...); what Zenix has no use for says so. Different there:
Unix paths (`/bin/sh`, no system-place check), `ps` shows pids and
`kill 123` takes one, `put --mode 755`, and two of its own:

```
wasabi --port 1236 text                     the console, as text
wasabi --port 1236 wait text 'login:'       until a line matches
wasabi --port 1236 reboot --to amigaos --yes --wait
                                            back to AmigaOS, and wait
                                            until its wasabid answers
```

A key and plain per machine, in the config (`~/.config/wasabi/config`),
so `--key` and the "no encryption" warning are not needed every time:

```
key@192.0.2.20:1236 = the-zenix-key
plain@192.0.2.20:1236 = yes
```

`plain@` is optional: the client remembers a port that turned out to
have no encryption by itself (see [Security](security.md)).
