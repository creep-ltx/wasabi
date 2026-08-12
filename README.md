# Wasabi

Drive a real Amiga from a Linux box. Upload a binary anywhere on the
system, run it and watch its output arrive live, and reboot the machine —
from the same terminal the code was written in.

Two halves:

- **`wasabid`** — a small daemon on the Amiga (C, Bebbo's gcc, one file).
- **`wasabi`** — the client (Python 3, stdlib only, one file). At home
  on Linux, macOS, FreeBSD and OpenBSD; elsewhere discovery's subnet
  sweep degrades to plain broadcast and `--host` always works.

```
$ wasabi discover
192.168.68.109  :1234  a1200        wasabid 0.1

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
| `grab` / `screen` | **working on the real A1200** — front screen to PNG in 0.1 s; RTG and native paths both |
| `name` + `ENV:HOSTNAME` discovery | **working on the real A1200** — it answers as `a1200`, not "an amiga" |
| stream heartbeat + farewell | **working on the real A1200** — a dead machine is noticed; a deliberate exit says goodbye first |
| exit guards + `--force` | **working on the real A1200** — no exit with a runner alive; `--force` Ctrl-Cs it and delivers its real exit code |

First live run: 12 August 2026, against an A1200 + PiStorm32-lite/CM4 on
Emu68 with [rondoval's driver stack](https://github.com/rondoval/emu68-driver-stack)
(lwIP `bsdsocket.library` over `genet.device`). Kickstart 47.115, 3.4 ms
round trip, a 19372-byte binary round-tripped byte-identical, `List
SYS:C` streamed all 119 entries, and a failing command reported
`rc 10, IoErr 205` correctly.

The client is additionally exercised end to end by `make test` against a
host mock that speaks the same protocol — 50 tests, no Amiga required.

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

**Amiga.** Build the daemon first — `make` with Bebbo's cross-compiler
(see *Building and testing*); the binary itself is not kept in git.
Then copy `wasabid` to `C:`, set a key, and start it:

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

**Linux or macOS.** Drop the client somewhere on `$PATH`:

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
wasabi info                  version, RAM, volumes, and what it can do
wasabi ls [PATH]             list a drawer; no path lists the volumes
wasabi put LOCAL REMOTE      upload a file
wasabi get REMOTE LOCAL      download a file ('-' for stdout)
wasabi run "CMD"             execute, stream output, return its exit code
wasabi deploy L R [--run C] [--reboot | --restart]
wasabi del PATH / mkdir PATH
wasabi reboot [--cold]
wasabi restart [--force]     reload the daemon in place (a debug tool -
                             'update' is the verified path)
wasabi update LOCAL          replace the daemon itself, verifying first
wasabi quit --yes [--force]  stop the daemon (needs physical access after)
wasabi debug [--with-snoop] [--log F]   live KPrintF stream
wasabi snoop [--task PAT] [--log F]     live DOS call trace
wasabi ps [PATTERN]          list every task; AmigaDOS wildcards filter
wasabi kill NAME|0xADDR      Ctrl-C a task; --force for RemTask
wasabi speedtest [SIZE] [--target PATH]  latency and throughput both ways
wasabi grab [FILE]           grab the front screen as a PNG
wasabi screen [--cycle|--to-front T]    list screens, flip between them
```

`--host` overrides discovery, `WASABI_HOST`/`WASABI_KEY` override the
config file. The daemon takes `wasabid [port] [name ID] [allow
CIDR|any]`, and is LAN-only unless told otherwise. `name` is what
discovery replies call the machine — two Amigas on one LAN stop being
interchangeable "amiga"s — and it overrides `ENV:HOSTNAME`, which is
the fallback (then plain `amiga`). It survives `restart` and
self-update, like the allow-list:

```
Run >NIL: C:wasabid name a1200
```

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

**A VPN subnet router keeps working, and you should know why.** Tested
on this network: a laptop on a phone hotspot, reaching the Amiga over
Tailscale via a NAS advertising the LAN as a subnet route, connected
with nothing configured and nothing refused. Tailscale masquerades
subnet-route traffic by default, so the Amiga sees the *router's* LAN
address rather than the `100.64.0.0/10` one the laptop holds — and
`192.168/16` already covers that.

The consequence is worth saying out loud: with a subnet router in the
path, **everything on your tailnet reaches wasabid as though it were on
the LAN**, and the daemon cannot tell the difference. That is not a hole
— a tailnet is authenticated and you choose who is in it — but your
tailnet membership is now part of wasabi's trust boundary. Worth
remembering before sharing a node, rather than after. (`allow` is still
there for a VPN that does *not* masquerade, where the Amiga would see
the real `100.x` source.)

Which of those is happening is observable rather than guesswork:
`wasabi debug` names every connection as it arrives.

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
wasabid 0.1 - 3.2 ms
```

Accepted connections are announced on the `debug` stream too, so "who
is actually talking to my Amiga" is answerable by watching rather than
inferring:

```
[wasabi: 192.168.68.117 connected]
[wasabi: refused 203.0.113.44 - not on the LAN]
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

**A stream can now tell a quiet machine from a dead one.** A reset or
frozen Amiga sends no FIN, so a `wasabi debug` left open across a reboot
used to sit forever looking connected. Two additions close that: the
daemon sends an invisible heartbeat — an empty `LOG` frame, rendering as
nothing — on every subscribed stream every ~5 seconds, and a deliberate
exit (reboot, restart, quit) says goodbye out loud first:

```
[wasabi: rebooting - closing this stream]
```

Twenty silent seconds and the client warns that the machine is frozen,
rebooted, or busy with something long — a warning rather than an exit,
because a big `put` in another terminal starves the single-threaded
daemon's heartbeats for exactly as long as it runs, and the stream picks
back up when it finishes. TCP keepalive on every connection is what
finally declares a truly dead machine dead, about half a minute in.

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
updating to wasabid 0.1
wasabid -> C:wasabid.new (44244 bytes)
  identity   wasabid 0.1 (2026-08-12)
  selftest   ok
  live       wasabid 0.1 served a handshake and a ping on port 1235
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
wasabid 0.1, protocol v1
exec.library 47.13
chip free 2020 KB, fast free 1883101 KB
volumes:
  RAM Disk:        1840 MB total   1840 MB free    0% used
  Dump:           32892 MB total  31289 MB free    4% used
  Crap:           20471 MB total  20471 MB free    0% used
  Work:            2860 MB total   2775 MB free    2% used
  AmigaOS:         1901 MB total   1877 MB free    1% used
can: debug del get grab hb info install kill ls mkdir ping ps put quit
     reboot restart run screen snoop speed speedfile

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

## Seeing the screen

`grab` is the verb, `screen` is the thing you operate on.

`wasabi grab shot.png` grabs the frontmost screen and writes a PNG
locally. 1280x960 in **0.1 seconds**, which is a design decision rather
than luck:

```
$ wasabi grab shot.png
1280x960 -> shot.png (1362 KB, 3.7 MB raw in 0.1 s)
```

**The Amiga sends raw pixels and does no compression at all.** Every
measurement in this file says the same thing — the wire is the fastest
part of this machine. 109 MB/s on the network against 20 MB/s from disk,
20 MB/s of ChaCha20, and a CPU that is the bottleneck behind every slow
thing here. A 1280x960 screen is 3.7 MB, which is thirty milliseconds of
wire and far less than the Amiga would spend deflating it. So the Amiga
reads its framebuffer and stops; the Linux box, idle and fast, makes the
PNG. Doing the "wasteful" thing is several times quicker end to end than
a screen grabber that compresses on the Amiga first.

**It grabs the frontmost screen, whatever that is**, and picks how to
read it from the screen's own depth — because no single call covers
both cases:

| Screen | Method | Measured |
|---|---|---|
| deeper than 8 bits | CyberGraphX `ReadPixelArray()`, true RGB | 1280x960 in **0.1 s** |
| 8 bits or fewer | `ReadPixelArray8()` + `GetRGB32()` palette | 640x256 in **2.0 s** |

CyberGraphX's autodoc says plainly that `ReadPixelArray` "should only be
used on screens depths > 8 bits", which rules it out for every native
Amiga screen — AGA stops at 8 bitplanes. So below that the pixels come
from `graphics.library` as pen numbers and the screen's palette turns
them into RGB. That path needs no RTG stack at all, so it also covers a
stock machine with no Picasso96.

The native path is far slower per byte — `ReadPixelArray8()` does a
planar-to-chunky conversion on the CPU, and the CPU is this machine's
slow part — but a PAL screen is a fifth the size, so it still lands in
about two seconds.

```
$ wasabi screen
ADDR       SIZE        DEPTH  TITLE
0x09265ee8 640x256         2  CygnusEd Professional V4.2   <- front
0x0829b3e0 1280x960       24  Workbench Screen

$ wasabi screen --cycle                 # exactly what Amiga+M does
$ wasabi screen --to-front "Workbench Screen"
```

Flipping screens is `ScreenToBack()`/`ScreenToFront()`, not synthesised
keystrokes into `input.device` — naming a screen beats cycling blindly
through them, and it keeps wasabi out of the business of injecting input.

One race is worth stating: the front screen is read through
`LockIBase()`, which makes reading the pointer safe but does not stop
the screen closing afterwards. Grabbing a screen that is being closed
during those few milliseconds would read freed memory. A *named* public
screen is properly locked and has no such window.

The PNG encoder on the client is about fifteen lines around `zlib`,
which is in the standard library — so this costs no dependency.

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

**0.1 is the first non-beta build.** Thirty-one betas led here, and the
last stretch was a full audit — `audit.md`, 12 August 2026 — that read
every line of both halves and the tests. Everything it called a bug is
fixed and verified on the machine: the out-of-bounds client-table
write, the orphaned-runner race, reboot honouring its own
documentation, timezone-straight `ls` dates, and the exit-with-a-live-
runner Guru it found on the way out. What remains in it is choices,
written down as such.

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
