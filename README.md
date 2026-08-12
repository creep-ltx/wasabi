# Wasabi

Drive a real Amiga from a Linux box. Upload a binary anywhere on the
system, run it and watch its output arrive live, and reboot the machine —
from the same terminal the code was written in.

Two halves:

- **`wasabid`** — a small daemon on the Amiga (C, Bebbo's gcc, one file).
- **`wasabi`** — the Linux client (Python 3, stdlib only, one file).

```
$ wasabi discover
192.168.1.42    :1234  amiga        wasabid 0.1b17

$ wasabi deploy ccon-handler L:ccon-handler --reboot
ccon-handler -> L:ccon-handler (106912 bytes)
rebooting the Amiga...

$ wasabi run "Version SYS:C/List FULL"
List 45.16 (2.7.2021)
```

The point is the loop: **edit on Linux, build with the cross-compiler,
and land it on real silicon without touching an SD card.**

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
| `snoop` (DOS call trace) | **working on the real A1200** — SnoopDOS-style patches on 14 dos/exec calls |
| `ps` / `kill` | **working on the real A1200** — full task list; Ctrl-C or RemTask |
| `speedtest` | **working on the real A1200** — gigabit line rate both ways at 256 MB |

First live run: 12 August 2026, against an A1200 + PiStorm32-lite/CM4 on
Emu68 with [rondoval's driver stack](https://github.com/rondoval/emu68-driver-stack)
(lwIP `bsdsocket.library` over `genet.device`). Kickstart 47.115, 3.4 ms
round trip, a 19372-byte binary round-tripped byte-identical, `List
SYS:C` streamed all 119 entries, and a failing command reported
`rc 10, IoErr 205` correctly.

The client is additionally exercised end to end by `make test` against a
host mock that speaks the same protocol — 45 tests, no Amiga required.

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

## Installing

**Amiga.** Copy `wasabid` to `C:`, set a key, and start it:

```
Copy wasabid C:
SetEnv SAVE wasabi.key hunter2
Run >NIL: C:wasabid
```

Put those last two lines in `S:User-Startup` to have it up after every
boot. Stop it with `Break <n> C`.

It needs a working `bsdsocket.library` — on a PiStorm32/CM4 that means
[rondoval's emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack)
(lwIP over `genet.device`), or Roadshow, or AmiTCP. Anything that
provides the standard API.

**Linux.** Drop the client somewhere on `$PATH`:

```
ln -s $PWD/wasabi ~/.local/bin/wasabi
mkdir -p ~/.config/wasabi
printf 'key = hunter2\n' > ~/.config/wasabi/config
```

No host is needed in the config — the client finds the Amiga by
broadcast and caches the answer.

## Commands

```
wasabi discover              find Amigas on this network
wasabi ping                  round-trip time and daemon banner
wasabi info                  Kickstart version, RAM, and what it can do
wasabi ls [PATH]             list a drawer; no path lists the volumes
wasabi put LOCAL REMOTE      upload a file
wasabi get REMOTE LOCAL      download a file ('-' for stdout)
wasabi run "CMD"             execute, stream output, return its exit code
wasabi deploy L R [--run C] [--reboot | --restart]
wasabi del PATH / mkdir PATH
wasabi reboot [--cold]
wasabi restart               reload the daemon in place
wasabi update LOCAL          replace the daemon itself, verifying first
wasabi quit --yes            stop the daemon (needs physical access after)
wasabi debug [--with-snoop] [--log F]   live KPrintF stream
wasabi snoop [--task PAT] [--log F]     live DOS call trace
wasabi ps [PATTERN]          list every task; AmigaDOS wildcards filter
wasabi kill NAME|0xADDR      Ctrl-C a task; --force for RemTask
wasabi speedtest [SIZE]      latency, then throughput both ways (max 256MB)
```

`--host` overrides discovery, `WASABI_HOST`/`WASABI_KEY` override the
config file. The daemon takes `wasabid [port] [allow CIDR|any]`, and is
LAN-only unless told otherwise.

## Four things the Amiga side gets right on purpose

**`put` is atomic.** The daemon writes to a sibling `.wasabi-tmp` and
renames over the target at the very end. An interrupted upload can
never leave a half-written binary where a working one used to be —
which matters when the target is `L:` and the machine is about to boot.

**`run` redirects to a temp file opened `MODE_READWRITE`, not
`MODE_NEWFILE`.** That one flag is the whole trick: `MODE_NEWFILE` takes
an *exclusive* lock, so the daemon could never open the file to tail it.
`MODE_READWRITE` is a shared lock and both processes can hold it.

**Only one `run` at a time.** A deliberate limit — it makes the handoff
to the runner process unambiguous, and one developer driving one Amiga
has no use for two concurrent builds. A second `run` gets a clear error,
not a queue.

**No blocking read or write is unbounded.** wasabid is one process with
one loop, so a single wedged peer used to take the machine with it: send
half a frame and stall, or subscribe to `debug` and stop reading, and the
daemon blocked in `recv()` or `send()` forever — nothing else served, not
even Ctrl-C honoured. Every blocking read and write now waits on
`WaitSelect()` with a ten-second bound (and watches Ctrl-C while it
does), then drops that client. Measured on the A1200: a client that sent
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

## Security

`wasabid` executes arbitrary commands as whatever started it, over a
plaintext socket, on a machine with no memory protection. The key in
`ENV:wasabi.key` stops an accidental connection from a misconfigured
tool; it stops nothing else.

**It answers only addresses that cannot be routed in from the
internet** — RFC1918 (`10/8`, `172.16/12`, `192.168/16`) plus loopback
and link-local. Anything else is closed before it can send `HELLO`. This
is not about hostile neighbours; it is about the mistake that actually
hurts a tool like this: somebody forwards port 1234, or their router
does it for them via UPnP, or the Amiga lands in a DMZ, and an
unauthenticated remote shell is now on the open internet. The check
lives in the daemon, where a router configuration cannot undo it.

Not narrower than RFC1918 on purpose. Restricting to `192.168` would
lock out every home whose ISP router hands out `10.0.0.x` — Xfinity's
default among others — and buy nothing, since `10/8` is exactly as
unroutable from outside as `192.168/16`.

Loopback is allowed deliberately: a connection arriving through an SSH
tunnel that terminates on the Amiga comes from `127.0.0.1`, so tunnels
and this check compose rather than fight.

A mesh VPN is a legitimate way to reach a machine and is not RFC1918
(Tailscale uses `100.64.0.0/10`), so ranges can be added:

```
Run >NIL: C:wasabid                          plain: RFC1918 + loopback
Run >NIL: C:wasabid allow 100.64.0.0/10      plus a Tailscale range
Run >NIL: C:wasabid allow any                off - warns loudly at startup
```

Refusals are counted per address rather than logged per event, because a
port scan would otherwise write a very large file about one host. The
tally lives in `L:wasabid.refused`, survives a restart, and shows up
where you will actually see it:

```
$ wasabi ping
wasabi: 673 connection(s) refused as off-LAN since you last looked
(673 in total) - 'wasabi debug' shows them as they happen
wasabid 0.1b17 - 3.2 ms
```

That warning appears only when the number has *risen* since you last
connected — a standing count on every command is wallpaper within a day,
whereas one that moves is a signal. `wasabi info` always shows the
total, and each refusal appears on the `debug` stream as it happens.

The discovery responder answers anyone *on the LAN*, deliberately —
needing the key to *find* the machine would defeat the point, and it
reveals no more than a port scan. It stays silent to everyone else.

## Building and testing

```
make            # cross-compile wasabid
make test       # client vs. the host mock - no Amiga needed
```

`tests/mock-wasabid.py` serves the same protocol out of a host directory.
It is both the test fixture and the reference the C daemon was written
against: where the two disagree about the wire, the mock is what the
client was proven with.

The cross-compiler is Bebbo's `m68k-amigaos-gcc`, at
`$(HOME)/opt/amiga/bin` on this box. Override `CC` if yours lives
elsewhere.

## The debug stream

`wasabi debug` streams every `KPrintF`/serial-debug byte the machine
produces, live over the network — the thing Sashimi captures, but built
in, no third-party tool. The daemon `SetFunction()`s exec's `RawPutChar`
(LVO -516) with its own patch, which runs in whatever context the caller
was in — task, interrupt, or Supervisor. So the patch allocates nothing,
holds only a short `Disable()`, does no I/O, and (like Sashimi) does not
chain to the original: it copies the byte into a 32K ring and returns.
The main loop drains the ring to `LOG` frames; dropped bytes on a full
ring are counted and reported, never silently lost.

Teardown removes the patch only if the vector still points at ours —
if another tool `SetFunction`'d on top, restoring the old pointer would
unlink *their* patch and crash the machine minutes later, so wasabid puts
its own patch back and stays installed rather than corrupt the chain.

Proven on the real A1200 (12 Aug 2026): a test program emitting via
`RawPutChar` streams through live, the patch installs and removes cleanly
across repeated sessions, and the machine stays healthy afterward.

## The snoop stream

`wasabi snoop` is the SnoopDOS trick delivered over the wire: the daemon
`SetFunction()`s fourteen of the `dos.library` and `exec.library` calls a
developer most wants to watch — `Open`, `Lock`, `LoadSeg`, `Execute`,
`SystemTagList`, `GetVar`, `SetVar`, `DeleteFile`, `Rename`, `CreateDir`,
`MakeLink`, `OpenLibrary`, `OpenDevice`, `FindPort` — and logs each call
with its caller, its string/numeric arguments and its real result:

```
$ wasabi snoop --task '#?'
Shell                Open("S:Startup-Sequence", read) = ok
Shell                Lock("DH0:Tools", read) = ok
cfile                LoadSeg("C:List") = ok
wasabid              GetVar("wasabi.key") = ok
```

`--task PAT` filters on the caller's name with AmigaDOS wildcards
(`#?`, `?`, `*`); no filter means everything. The caller is named the way
SnoopDOS names it — the CLI command being run if there is one, else the
task name.

Each patch is a tiny stub that pushes a descriptor and jumps to one
common trampoline. The trampoline runs in the **calling** task's context,
so — like the debug patch, and for the same reasons — the C it invokes
allocates nothing, holds only a short `Disable()`, and reads
`SysBase->ThisTask` directly rather than through any call it has patched,
on pain of unbounded recursion. Two more rules come straight from Eddy
Carroll's SnoopDOS source: the stub tests an enabled flag first and falls
through to the untouched original when snooping is off (so a patch that
*cannot* be removed idles at a few instructions), and it refuses to build
an event when the caller is low on stack, counting the drop instead of
overflowing. Unlike SnoopDOS, the log line is emitted **after** the
original returns, so every line carries the true result and `IoErr()`
rather than "pending".

**Every session self-tests before it patches anything.** The asm hands
the C a 60-byte register file and the descriptors say where in it each
argument lives — two definitions that must agree exactly, with no
compiler checking them. So `snoop` first `Lock()`s a bogus path of its
own and insists the captured event carries back that exact string, the
mode it passed and the result it got. If they disagree, snoop refuses to
run and says so, rather than dereference whatever was in the wrong
register — in another task's context, on a machine with no memory
protection. Verified by deliberately wiring one descriptor to the wrong
register: the daemon refused, stayed up, and the patches came back out.

Teardown follows the same chain rule as the debug patch — a stub whose
vector was chained over stays installed and idle rather than corrupt the
chain — and the exit path waits briefly for any task still inside a stub
before the code segment can unload. Events ride the same `LOG` frame as
debug output, on **stream 1** (debug is stream 0), so a monitor can tell
serial debug and the call trace apart.

Proven on the real A1200 (12 Aug 2026): `List SYS:C` traces its `Lock` /
`LoadSeg` and the `OpenLibrary` storm that follows, a `--task` filter
narrows the stream to one caller exactly, sessions open and close
repeatedly, and the machine stays healthy after teardown. The event ring
holds 512 entries (~107 KB) because a PiStorm-fast CPU can fire more than
150 patched calls between two 50 ms drains — the first cut held 64 and
`List SYS:C` alone overflowed it. A burst that still overflows is dropped
and counted, never silently lost.

For the full picture, `wasabi debug --with-snoop` merges both streams
into one terminal over a single connection — the daemon tags every `LOG`
frame with its stream, so each line arrives prefixed `debug |` or
`snoop |`. And `--log FILE` on either command appends every line to a
file stamped with the client's receive time to the millisecond, which is
what lets output captured in parallel terminals be lined up afterwards:

```
2026-08-12 08:11:53.257 snoop | c:wasabid  Open("T:wasabi-run-2", readwrite) = ok
```

## Updating the daemon

The daemon can replace itself: `LoadSeg` copies the binary into memory at
launch and holds no lock on the file, so the file can be swapped while it
runs and `restart` picks up the new one without a reboot. The catch is
that once the old daemon has exited to relaunch, **nothing is left
running that could undo a bad install** — a broken binary means walking
to the machine, which is the one thing wasabi exists to avoid.

So `put` refuses that path outright, whatever alias you spell it with:

```
$ wasabi put wasabid SYS:C/wasabid
wasabi: that is the running daemon - use 'wasabi update', which verifies
the binary before it commits
```

(The daemon compares with `SameLock()`, not by name, so `C:wasabid`,
`SYS:C/wasabid` and any assign that reaches the same file are all
recognised as itself.)

`wasabi update` is the one way in, and it earns the privilege with four
checks — cheapest first, each catching what the one before it cannot:

```
$ wasabi update wasabid
updating to wasabid 0.1b16
wasabid -> C:wasabid.new (32856 bytes)
  identity   wasabid 0.1b16 (2026-08-12)
  selftest   ok
  live       wasabid 0.1b16 served a handshake and a ping on port 1235
installed - the daemon is reloading itself
```

1. **Does the local file even claim to be wasabid?** It must carry a
   `$VER: wasabid` tag. A genuine `C:List` is refused here, before a
   single byte goes over the network.
2. **Does the uploaded copy identify as that same version?** Read out of
   the staged file by AmigaDOS's own `Version` command — nothing is
   executed yet.
3. **Does it run, and know something only wasabid knows?** It is invoked
   as `--selftest <nonce>` and must print back a marker carrying that
   nonce. Exit status alone would prove nothing: `C:Echo --selftest`
   prints its argument and exits 0, and *that* installed as the daemon
   is a machine off the network.
4. **Does it actually work as a daemon?** It is started on a spare port,
   and the client connects to it for real — handshake, banner check,
   ping — then tells it to quit.

Only then is it installed, by renaming the verified sidecar into place,
so the bytes that passed the checks are exactly the bytes that run. The
previous binary is kept as `C:wasabid.bak` — on the Amiga, not on the
Linux box, because if what breaks is the network then a human at the
keyboard is who needs it. Any failure deletes the sidecar and leaves the
running daemon untouched:

```
$ wasabi update some-other-build
updating to wasabid 9.9fake
  identity   wasabid 9.9fake (2026-01-01)
wasabi: it did not pass its own self-test (rc 10, said 'C:wasabid.new:
file is not executable'). The running daemon is untouched
```

Check 4 is the one that earns its keep, and it was proven on the A1200
with a wasabid built to bind its port and then exit immediately. It is a
genuine build: the right `$VER`, and a self-test that really does open
`bsdsocket.library` and bind a socket. Checks 1, 2 and 3 **all passed
it** — installing that binary would have taken the machine off the
network at the next restart. Only connecting to it caught it:

```
$ wasabi update wasabid-broken
updating to wasabid 0.1b16
  identity   wasabid 0.1b16 (2026-08-12)
  selftest   ok
wasabi: it did not come up as a daemon on port 1235 ([Errno 111]
Connection refused); a test instance may still be on port 1235 - find it
with 'wasabi ps' and stop it with 'wasabi kill'. The running daemon is
untouched
```

What none of this catches is a binary that passes every check and then
dies under real load. `--selftest` also deliberately does not exercise
the `SetFunction` patches: a process that patches the system and then
exits is the very hazard the teardown rules exist to prevent.

### Telling the client and daemon apart

Because updating is this easy, the client is usually a `git pull` ahead
of the daemon, and "unknown command" is a poor way to discover it. So
the daemon lists what it can do in its `WELCOME`, and `info` shows it:

```
$ wasabi info
wasabid 0.1b16, protocol v1
exec.library 47.13
chip free 1948 KB, fast free 1875667 KB
can: debug del get info install kill ls mkdir ping ps put quit reboot
     restart run snoop speed

$ wasabi speedtest 10MB      # against a build made without 'speed'
wasabi: this daemon (wasabid 0.1b16-nospeed) has no 'speed' - update it
with 'wasabi update wasabid'
```

Adding that list broke nothing in either direction, by construction:
frame lengths are explicit and the banner is a counted string, so an
older client stops before the list and never sees it, while a newer
client that finds no list treats it as *unknown* rather than *supports
nothing* and just sends the command. Both directions were checked
against the A1200 — a current client against a daemon with no list, and
a client checked out of git from before the list against one that sends
it.

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
instead of blending in a filesystem. (A future `--via PATH` mode could
measure through a real volume; it must first check `info`'s free-memory
numbers and refuse a size that does not fit with a healthy margin.)

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

The hardening list is done — snoop self-tests its own trampoline, the
daemon can only be replaced through `update`, and no blocking read or
write is unbounded.

Deliberately on hold: **challenge-response auth**. HMAC over a server
nonce would keep the key off the wire, but the session stays plaintext
anyway — auth without confidentiality, the most code of the five, against
a threat the trusted-LAN scope already declares out of bounds. The
current key is accident prevention and says so; that is a choice, not an
oversight.

`PROTOCOL.md` is the wire format, and the contract between the two halves.
