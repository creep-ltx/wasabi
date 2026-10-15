# Design, status and measurements

[Back to the README](../README.md)

## Status

| Piece | State |
|---|---|
| Discovery, handshake, `ping`, `info` | **working on the real A1200** |
| `put` / `get` / `ls` / `del` / `mkdir` | **working on the real A1200** |
| `run` with live output and real exit code | **working on the real A1200** |
| `deploy` (upload + run in one shot) | **working on the real A1200** |
| `reboot` | **working on the real A1200** |
| `debug` (KPrintF stream) | **working on the real A1200** — standalone, own RawPutChar patch |
| `restart` / `update` (self-update) | **working on the real A1200** — `update` verifies four ways before it commits |
| `snoop` (DOS call trace) | **working on the real A1200** — SnoopDOS-style patches on 30 dos, exec, icon and diskfont calls |
| `ps` / `kill` | **working on the real A1200** — full task list with stack headroom; Ctrl-C or RemTask |
| `speedtest` | **working on the real A1200** — gigabit line rate both ways at 256 MB |
| `grab` / `screen` | **working on the real A1200** — front screen to PNG in 0.1 s; RTG and native paths both |
| `desktop` | **working on the real A1200** (tested in a headless Firefox: every page, copies both ways with folders, delete, screenshots) — the desktop app: live stats, the screen, a two-pane file manager, screenshots, reboot |
| `view` | **working on the real A1200** (tested in a headless Firefox: picture, clicks, typing, Right Amiga shortcuts, right-button menus, settings) — the live screen in a window, driven with the PC's mouse and keyboard; only changes are sent, key-to-screen 52 ms over Wi-Fi |
| `health` | **working on the real A1200** — Pi temperature, voltage, clocks and its power/overheat history since boot; Emu68 uptime, a live 68k CPU meter (0.3 MIPS idle, 1287 MIPS busy, measured), JIT cache; free memory; the last guru |
| `slots` / `free` | **working on the real A1200 (0.4b2)** — four commands at once (one per connection); `wasabi slots` lists them, `wasabi free N` Ctrl-Cs one and lets go of the slot if it will not stop; `run --max-time` stops exactly its own slot |
| TLS (`keys`) | **working on the real A1200 and the NAS; optional since 0.8** — Ed25519 certificates from your own CA; an Amiga with AmiSSL and keys accepts only TLS, one without works plain; a client never silently goes plain to an Amiga it reached encrypted |
| `ui` (gadgets and menus by name) / `arexx` | **working on the real A1200; ReAction and MUI tested in FS-UAE (OS 3.2.3, MUI 5)** — list a window's gadgets and menus; click a button, fill a text field, pick "Project/Quit" by name; send ARexx commands and get RESULT. Intuition, GadTools, ReAction and MUI 4/5 windows (0.7b1), MUI 3.8 (0.10b3, FS-UAE OS 3.1) |
| list rows in `ui` | **working on the real A1200** (GadTools and MUI 5); DCTelnet's Address Book in FS-UAE — each row by its text, selected or not, clickable when in view |
| transfers that do not block | **working on the real A1200** — during a 200 MB upload, ping 0.16 s and a 1280x960 screenshot 0.5 s |
| `wasabid.020` | **working on the real A1200 (it runs that build) and in FS-UAE as a stock A1200** (68EC020, 8 MB) — the same daemon built for 68020+; `make both` |
| one-file clients | **0.9, built and smoke-tested by GitHub Actions** on Linux (x86-64, ARM64), macOS (ARM, Intel) and Windows; the Linux one also against the real A1200 |
| `--native` coordinates, `grab --region`, `mouse where`, more keys | **working on the real A1200** (pointer read back exactly, region grab) and in the client tests (261) |
| `sum`, `wait change/still` by checksum | **0.9: the real A1200's RTG screen** (whole 1280x960 in 0.19 s, stable) **and an emulated A600** (whole 640x256 in 0.8 s from the bitplanes; a grab takes 5.8 s) |
| AmigaOS 2.04+ | **0.9, FS-UAE as a stock A600 with Workbench 2.05** — files, run, windows, grab, sum, keys (å/ä/ö), ui including the About requester's OK; GadTools list rows need OS 3 |
| `WASABI` ARexx port | **working on the real A1200 (answers there); every command tested in FS-UAE** — Amiga programs and scripts type, press keys, click, click gadgets by name and pick menus through it |
| MCP server (`wasabi_mcp.py`) | **working** (tested in FS-UAE; registered for every Claude Code session on the author's PC) — the same tools for Claude Desktop, Codex, Gemini CLI, Cursor and other MCP clients |
| `mouse` / `key` / `windows` | **working on the real A1200** — click, drag and drive menus; type text through the Amiga's own keymap (Swedish å/ä/ö/@ checked byte for byte) and press keys by name; every screen and window as text |
| `name` + `ENV:HOSTNAME` discovery | **working on the real A1200** — it answers as `a1200`, not "an amiga" |
| stream heartbeat + farewell | **working on the real A1200** — a dead machine is noticed; a deliberate exit says goodbye first |
| stream auto-reconnect | client-side — a lost stream retries forever and says why it dropped; `--once` restores stop-on-disconnect |
| guru report | **working on the real A1200, for system alerts only** — names the alert code and the dying task on both streams, keeps the note in `T:lastguru`, and carries a deadend across a reboot in a top-of-RAM black box. **Does not catch a program faulting**: those go to `tc_TrapCode`, not `Alert()` |
| guru PC capture | **built, dormant on Emu68** — snapshots the supervisor stack at `Alert()` time and decodes the exception frame for a PC. Emu68 does not deliver CPU exceptions to the guest, so no frame arrives to decode |
| exit guards + `--force` | **working on the real A1200** — no exit with a runner alive; `--force` Ctrl-Cs it and delivers its real exit code |

First live run: 12 August 2026, against an A1200 + PiStorm32-lite/CM4 on
Emu68 with [rondoval's driver stack](https://github.com/rondoval/emu68-driver-stack)
(lwIP `bsdsocket.library` over `genet.device`). Kickstart 47.115, 3.4 ms
round trip, a 19372-byte binary round-tripped byte-identical, `List
SYS:C` streamed all 119 entries, and a failing command reported
`rc 10, IoErr 205` correctly.

The client is additionally exercised end to end by `make test` against a
host mock that speaks the same protocol — 255 tests, no Amiga required,
run on Linux and macOS for every push.

### Discovery on a Wi-Fi-to-wired network

The broadcast probe **did not work** on the machine this was built for:
the Linux box is on Wi-Fi, the Amiga on ethernet, and the bridge does not
carry broadcast between them — neither `255.255.255.255` nor the
subnet-directed address gets a reply, while a unicast probe to the same
port answers instantly.

So `discover` falls back to sweeping the local subnet with unicast
probes when broadcast finds nothing. 254 small UDP packets cost about a
second and always work. The sends are non-blocking, because a blocking
`sendto()` to an address with no ARP entry stalls until resolution gives
up — 254 of those in a row turned a 1-second probe into 8.

An emulator on the same PC does not count as "found": FS-UAE always
answers on `127.0.0.1`, and the A1200 often misses the broadcast, so
`discover` sweeps whenever only this PC answered (before 2026-10-10 it
stopped at the emulator and missed the A1200 two times in three). An
emulator also answers a broadcast from the PC's own LAN address; that is
folded into `127.0.0.1`, where it really listens.

**More than one emulator.** Give each wasabid its own port - a bare
number on its command line, `wasabid 1235` - and point the client at it
with `--port 1235` or `WASABI_PORT=1235`. `discover` probes loopback on
the default port and the three after it, so it lists each emulator.

**Which machine.** With no `--host`, the client uses the address it used
last. If that stops answering (the A1200 got a new address from DHCP
after a restart), it searches again - up to three times, since a machine
just back may not have its network up yet - and picks, in order: the
machine with the same name as last time; else the one real machine over
emulators on this PC; else it lists them and asks for `--host`.

## Four things the Amiga side gets right on purpose

**`put` is atomic.** The daemon writes to a sibling `.wasabi-tmp` and
renames over the target at the very end. An interrupted upload can
never leave a half-written binary where a working one used to be —
which matters when the target is `L:` and the machine is about to boot.
One honest caveat: AmigaDOS `Rename()` will not clobber, so the old file
is deleted an instant before the rename. Half-written, never; briefly
absent if the machine dies inside that instant, possible.

**`run` redirects to a temp file opened `MODE_READWRITE`, not
`MODE_NEWFILE`.** That one flag is the whole trick: `MODE_NEWFILE` takes
an *exclusive* lock, so the daemon could never open the file to tail it.
`MODE_READWRITE` is a shared lock and both processes can hold it.

**One thing at a time, and a big transfer really means it.** A `PUT` is
served inside the receive loop, so the daemon does not return to
`select()` until the whole file has arrived — nothing else is answered
meanwhile. At 30 KB that is 3 ms and invisible; a 4 GiB upload locks
everything out for five minutes, and other clients time out rather than
being refused. The honest fix is the per-client input buffer that
`recv_frame`'s comment describes, which still is not warranted for one
developer and one Amiga — but it is a real cost, not a theoretical one.

**Only one `run` at a time.** A deliberate limit — it makes the handoff
to the runner process unambiguous, and one developer driving one Amiga
has no use for two concurrent builds. A second `run` gets a clear error,
not a queue.

**No blocking read or write is unbounded.** wasabid is one process with
one loop, so a single wedged peer would otherwise take the machine with
it: send half a frame and stall, or subscribe to `debug` and stop
reading, and nothing else gets served. Every blocking read and write
waits on `WaitSelect()` with a ten-second bound, watches Ctrl-C while it
does, and then drops that client. Measured on the A1200: a client that sent
half a frame and went silent was cut off after 10.0 s, a `ping` issued
one second into the stall was answered 9.0 s later — the remainder of the
timeout — and everything after it was instant again. Ten seconds is
enormous next to an honest frame; 64 KB crosses this network in under a
millisecond, and throughput is unchanged at line rate.

## Things that are the way they are because AmigaOS is

**stderr is not separable.** OS 3.x's `SystemTagList()` has `SYS_Input`
and `SYS_Output` and no `SYS_Error` — a command's errors go to the same
stream as its output. So `run` sends everything as `STDOUT`. The
protocol keeps a `STDERR` tag for a future OS4 or custom-shell route;
today it is never sent. (The mock *does* separate them, which is why the
test suite checks it — the mock is Unix underneath.)

**Liveness is bounded by the child's own buffering**, exactly as on Unix.
A C program writing through buffered stdio to something that is not a
console emits in lumps no matter what the daemon does. AmigaDOS commands
that write with `Write()` stream immediately.

**`T:` should be in RAM** for `run` to feel live. It normally is.

**Files stop at 4 GiB − 1, and that is the wire, not a buffer.** `PUT`
carries the size in a `u32` and the daemon counts arrivals in a `ULONG`,
so the client refuses anything larger up front rather than truncating
it. Going past that needs a 64-bit size — a new tag behind a capability
— not a bigger buffer. `GET` has no size field and streams until `END`,
so downloads have no such limit.

The filesystem usually gives up first anyway: PFS3 caps a file at 4 GB,
so only an SFS\2 volume can hold the largest thing wasabi can send.
Measured on the A1200, both directions on the same SFS\2 volume:

| | Size | Time | Rate |
|---|---|---|---|
| `put` (write) | 4,294,967,295 | 317 s | 13.5 MB/s |
| `get` (read) | 4,831,838,208 | 232 s | 20.8 MB/s |

Reads run 1.5× faster, which is the expected direction — writing also
allocates blocks and updates metadata. Both are disk-bound: the same
network moves 109 MB/s, so wasabi is 5–8× away from being the limit.

That read was a **4.5 GiB file, larger than `put` can send**, which is
the asymmetry above demonstrated rather than argued: `get` declares no
size and simply streams until `END`.

**Sizes are read unsigned, and still wrap past 4 GiB.** OS 3.x's
`Examine()` returns `fib_Size` as a *signed* 32-bit LONG, so a 4 GB file
reports as `-1`. Since a file cannot be negative bytes long, `ls` reads
it unsigned, which is correct to 4 GB — the same ceiling `put` has.

Beyond that nothing can save it: that 4.5 GiB file lists as 536,870,912
bytes, because `fib_Size` is the true size modulo 2³², and 512 MiB is
far too plausible to detect. Reporting it properly needs the 64-bit
filesystem extension (`ACTION_GET_FILE_SIZE64`, which SFS\2 supports),
but that means opening every file to ask — far too expensive for a
directory listing. So `ls` is honest to 4 GB and wrong above it, and
that is written down here rather than discovered later.

**Nobody is at that keyboard, so nothing may ask a question.** A DOS
requester — "Please insert volume Work: in any drive" — is not a
question when the machine is being driven over a network; it is a wedge.
It cost a run slot and 128 KB of stack, permanently, until someone
walked over and clicked. Both the daemon and each runner set
`pr_WindowPtr = -1`, so DOS fails the call instead and the error comes
back down the wire: `List DF0:` now returns `rc 20, IoErr 226 - no disk
in drive` rather than hanging forever.

**Some programs escape `run`'s output capture.** `run` redirects
`SYS_Output`, and a program that writes to its own stream instead of the
process's `Output()` handle will not be caught — AmiSSL's `OpenSSL` is
built `-DOPENSSL_NO_STDIO` and does exactly this, printing nothing
through wasabi while working perfectly. The workaround is to redirect on
the Amiga side and fetch the file: `run "cmd >T:out"` then `get T:out -`.

## Speedtest

`wasabi speedtest 25MB` measures latency first — 200 `PING`/`PONG` round
trips back to back (`--pings N` adjusts, 0 skips), each one timed, so
the line shows min/max/jitter and not just a flattering average:

```
ping    4.14 ms     min 2.14 / max 55.41 / jitter 8.11  (200 pings)
up     81.53 MB/s   10.0 MB in 0.12 s
down   63.92 MB/s   10.0 MB in 0.16 s
```

(That 55 ms outlier against a 2 ms floor is the Wi-Fi hop on the client
side of this network, caught in the act — the reason the spread is
worth printing.)

Then it pushes, and pulls, the given number of bytes and reports MB/s
each way — so the effect of a driver stack or MTU change is one command
to measure. The daemon counts-and-discards on receive and
generates on send: nothing is stored anywhere, a stock 2 MB machine runs
the same test a PiStorm does, and the figure isolates the network path
instead of blending in a filesystem.

`--target PATH` writes through a real volume instead of discarding, so
the same command measures the filesystem rather than the network. It
asks the daemon for the volume's free space first and refuses rather
than filling it — the obvious mistake, `speedtest 256MB --target RAM:`
on a machine with 66 MB, would otherwise consume memory until something
important failed to allocate:

```
$ wasabi speedtest 100MB --target Dump:
wasabi: cannot speedtest to Dump:: needs 101 MB plus 8 MB spare, and
only 63 MB is free
```

Measured on the A1200, 50 MB each way, three runs:

| Target | up MB/s | down MB/s |
|---|---|---|
| wire only (discard) | 97.9 | 77.1 |
| `RAM:` | 42 | 42–55 |
| `Dump:` (PFS3) | 13.3 | 20.0 |
| `Crap:` (SFS\2) | 12.6–13.5 | 20.0 |

**Do not use the machine while measuring.** The daemon is single
threaded, so a `speedtest` never returns to `select()` until it
finishes — another client's request queues behind it, and the time
spent serving that request lands inside the timing, because the client
measures from request to `END`. This is not hypothetical: the first
`RAM:` read here came out at 17.9 MB/s, slower than PFS3, which was
obviously wrong. It was a concurrent `wasabi info` from another
terminal, and three repeats put the real figure at 42–55. If a number
looks impossible, check whether anything else was talking to the Amiga
before blaming the filesystem.

PFS3 and SFS\2 are indistinguishable here. `RAM:` is about three times
faster than either — but still less than half the wire, so more than
half of a RAM disk's theoretical speed goes on `ram-handler` and the DOS
write path rather than on memory. The read figures line up with the
4.5 GiB download measured separately at 20.8 MB/s, across a hundredfold
change in file size.

On this network the ceiling turns out to be the wire itself — a 256 MB
test runs the A1200 at gigabit line rate both ways. The stack is
[rondoval's emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack)
on his rangeops build of Emu68 1.1alpha2, and the rangeops part is what
buys the line rate — the stock build tops out around 104 Mb/s:

```
ping    2.33 ms     min 2.14 / max 3.99 / jitter 0.31  (200 pings)
up    109.12 MB/s   256.0 MB in 2.35 s
down  105.02 MB/s   256.0 MB in 2.44 s
```

## What's next

**0.10 is the fifth non-beta build** (15 October 2026): the 0.10
betas below - timed input batches, gadgets by id, `wasabi rexx`, the
Python API (`wasabi_client.py`), MUI 3.8 windows and the OS 3.0/3.1
fixes. CHANGELOG.md has every version.

**0.9 was the fourth non-beta build**: ready-made clients for Linux,
macOS and Windows, list rows, transfers that do not block, a 68020
build, more keys, region grabs, `--native` coordinates and 26 MCP tools.
Updated the same day (15 October 2026, at the user's wish, under the
same number): `wasabi sum` and the `amiga_region_changed`, `amiga_slots`
and `amiga_break` MCP tools; `wait change/still` no longer grab the
screen; wasabid runs on OS 2.04+ (it called four graphics.library 39
functions unguarded - `windows` on Workbench 2.05 took the daemon down;
tested on an emulated stock A600); `ui` also reads gadgets inside
requesters, where OS 2.x's system requesters keep their buttons; and
wasabid reserves 250 KB less memory (the trace, live-view and ARexx
buffers are taken when first used).

**0.8 was the third.** Since 0.2: the keyboard, windows
and screenshots; the live view, the desktop app and the phone app with
history, alerts and nightly backups; health; four run slots; gadgets and
menus by name in Intuition, GadTools, ReAction and MUI windows; ARexx
both ways; the MCP server; and optional TLS with your own certificates.

Since 0.8 (0.9 betas): list rows; uploads and downloads that no longer
make other clients wait; a 68020 build; `wasabi info` names the patches
in place.

Since 0.10 (betas): MUI 3.8 windows are read (0.10b3); the daemon runs
on a 16 KB stack of its own when the Shell gave it less (OS 3.1 gives
4 KB, which crashed `ui` there); GadTools list rows on OS 3.0/3.1 too.
0.10b4, from spotUP's list (UP-Term's test rig): `INPUTS`, a timed
batch of key and mouse events played by the Amiga (`wasabi input`);
gadgets by id (`GadgetID`, MUI's `MUIA_ObjectID`; `ui click --id`);
`wasabi rexx` runs ARexx programs through `RX` with output and rc;
`wasabi_client.py`, the stable Python API (docs/api.md, API 1.0);
clearer "make it plain again" errors; menus no longer show plain items
as checked. Tested in FS-UAE (OS 3.2.1 + MUI 5, Workbench 2.05) and on
the A1200.

Open: MUI lists' columns after the first,
and several listeners on the debug and snoop streams at once.

The hardening list is done — snoop self-tests its own trampoline before
it patches, the daemon can only be replaced through `update`, no
blocking read or write is unbounded, and a patch that cannot be removed
keeps the binary resident rather than unloading into a guru.

`docs/audits/audit*.md` are the defect ledgers, one per
line of development. `PROTOCOL.md` is the wire format, and the contract
between the two halves.
