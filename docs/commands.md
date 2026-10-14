# Commands

[Back to the README](../README.md)

## Commands

```
wasabi discover              find Amigas on this network
wasabi ping                  round-trip time and daemon banner
wasabi info                  version, RAM, volumes, and what it can do
wasabi ls [PATH]             list a drawer; no path lists the volumes
wasabi put LOCAL REMOTE      upload a file (--force in a system place)
wasabi get REMOTE LOCAL      download a file ('-' for stdout)
wasabi run "CMD" [--max-time S] [--detach]
                             execute, stream output, return its exit code;
                             Ctrl-C it if it runs past S seconds; --detach
                             starts it and returns at once (output to T:)
wasabi clip get | set [TEXT|-]
                             read or set the Amiga's clipboard
wasabi deploy L R [--run C] [--reboot | --restart] [--force]
wasabi del PATH [--force] / mkdir PATH
wasabi reboot --yes [--wait [S]]  reboot; --wait until it answers again
wasabi restart [--force]     reload the daemon in place (a debug tool -
                             'update' is the verified path)
wasabi update LOCAL          replace the daemon itself, verifying first
wasabi quit --yes [--force]  stop the daemon (needs physical access after)
wasabi debug [--with-snoop] [--log F] [--for S] [--until TEXT]
                             live KPrintF stream
wasabi snoop [--task PAT] [--log F] [--for S] [--until TEXT]
                             live DOS call trace
wasabi ps [PATTERN]          list every task, with stack headroom;
                             AmigaDOS wildcards filter
wasabi kill NAME|0xADDR      Ctrl-C a task; --force for RemTask. NAME
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
wasabi wait window|task|file|change|still|up [NAME] [--gone]
            [--contains TEXT] [--timeout S]
                             wait for something instead of sleeping
wasabi mouse where           where the pointer is (x y)
wasabi mouse move|click|doubleclick|down|up [X Y] [--right|--middle]
                             move the pointer, click, drag, drive menus
wasabi key type TEXT [--enter]
                             type text, through the Amiga's own keymap
wasabi key press KEY...      press keys by name: return, esc, f1-f12, up,
                             home, end, pageup, pagedown, insert, kp0-kp9,
                             kpenter, ramiga+w, ctrl+c, or a code: 0x45
wasabi ui list|menus [--window T] [--all-screens]
                             gadgets and menu items by name
wasabi ui click NAME [--window T] [--kind K] [--nth N] [--double]
                             click a gadget by its label (or close,
                             depth, zoom, size, drag)
wasabi ui set NAME TEXT [--no-enter]   fill a text field
wasabi ui menu PATH [--window T]       pick 'Project/Save As' or 'Quit'
wasabi ui front TITLE        bring a window to the front, activate it
wasabi keys init|amiga NAME|drive DIR NAME|show|forget HOST
                             the encryption keys (see Security)
wasabi mcp                   the MCP server on stdin/stdout, for AI
                             assistants (see Scripts and AI)
wasabi arexx PORT COMMAND [--timeout S]
                             send an ARexx command, print RESULT; exit
                             code = its rc. No PORT: list the ports
```

`--native` (or `coords = native` in the config, or
`WASABI_COORDS=native`) makes every coordinate the screen's own pixels;
see [coordinates](screen-and-gui.md#coordinates).

Every command also takes `--json` (where it lists something),
`--plain` (no encryption this time) and `--silence S` — see [Made for scripts](scripting-and-ai.md#made-for-scripts-and-for-claude).
`--host` overrides discovery, and `WASABI_HOST`/`WASABI_PORT`/`WASABI_KEY`/
`WASABI_SILENCE` override the config file. The daemon takes `wasabid [port] [name ID] [allow
CIDR|any] [plain]`, and is LAN-only unless told otherwise; `plain` lets
unencrypted clients in even when it has TLS. `name` is what
discovery replies call the machine — two Amigas on one LAN stop being
interchangeable "amiga"s — and it overrides `ENV:HOSTNAME`, which is
the fallback (then plain `amiga`). It survives `restart` and
self-update, like the allow-list:

```
Run >NIL: C:wasabid name a1200
```
