# Debugging on the Amiga

[Back to the README](../README.md)

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

**A stream tells a quiet machine from a dead one.** A reset or frozen
Amiga sends no FIN, so a `wasabi debug` left open across a reboot would
otherwise sit forever looking connected. Two things prevent that: the
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

**And a stream, once open, stays open.** Whatever kills the connection —
keepalive giving up on a frozen machine, a reboot's reset, a Wi-Fi hop
blinking overnight, the daemon restarting for a self-update — the client
announces the loss and then retries every couple of seconds, forever:

```
[debug+snoop: connection lost (connection closed by the Amiga) - reconnecting until the Amiga returns; Ctrl-C to stop]
[debug+snoop: reconnected after 34 s - wasabid 0.2]
```

then resubscribes and streams on. `--once` restores the old
stop-on-disconnect behaviour for scripts.

A `--log` file also records the client's own side of the story as
`client |` lines — how the stream was started and with which options,
every loss and reconnect, and how it ended — so a log read weeks later
says for itself what was subscribed and when:

```
2026-08-13 09:14:20.101 client | debug+snoop stream open to 192.168.1.20:1234 (wasabid 0.2) - Ctrl-C to stop
```

## The guru report

A guru is the one event a stream cannot describe by itself: the snoop
trace stops mid-thought, and which task died with which alert code stays
on the Amiga's frozen screen. `wasabid` patches exec's `Alert()` (LVO
-108) for its whole lifetime, so an alert reaches both streams before the
original freezes the display:

```
snoop | c:wasabid            Rename("C:wasabid", "C:wasabid.bak") = ok
snoop | [wasabi: DEADEND ALERT #81000005 in task 'c:wasabid' (0x08298d08)]
```

**Scope: this catches the system's own alerts, not a crashing
application.** A program that takes a CPU exception goes to its task's
`tc_TrapCode` — for a Process, dos.library's "Software Failure"
requester — and never touches the `Alert()` vector. What the hook does
see is exec raising an alert of its own (`AN_MemCorrupt` and friends)
and any program that calls `Alert()` deliberately.

A deadend may take the machine down before the daemon can get the line
onto the wire, so the report does not depend on the live stream. Exec's
`ExecBase->LastAlert` does not survive a reboot on Emu68, but plain RAM
at the top of the fast pool does — so at startup the daemon
`AllocAbs()`es 512 bytes just under `mh_Upper` of the highest fast-RAM
MemHeader, and the patch stashes the report there (code, task name,
address, a stack snapshot, checksum) for the next boot to find.
**Owning that region is not optional**: free memory on AmigaOS holds the
allocator's own `MemChunk` headers in its first eight bytes, so writing
into memory you have not reserved corrupts the free list and gurus the
machine later at an unrelated `FreeMem`. `LastAlert` is still checked
too, for hardware where exec preserves it.

Every captured alert also becomes one line of plain text in
`T:lastguru`, stamped with when it was seen, and every new `wasabi
debug` or `wasabi snoop` replays it as its first line:

```
[wasabi: last guru: DEADEND ALERT #81000005 (exec: corrupt memory list detected in FreeMem) in task 'c:wasabid' (0x08298d08) - 13-Aug-26 19:27:12]
```

The client names any code it recognizes from `exec/alerts.h` — `ALERT
#80000004 (CPU: illegal instruction)`, `#8100000F (exec: bad address
passed to FreeMem)` — the same way it dresses DOS error codes. One it
cannot parse stays bare rather than guessed at.

`T:` is RAM-backed, so the note survives daemon restarts and
self-updates and dies with the boot, which is exactly when the black box
takes over. `Type T:lastguru` reads the same note on the Amiga.
Auto-reconnect closes the loop: the machine gurus, reboots, wasabid
comes back, the stream resubscribes, and the alert arrives in the same
terminal that watched the machine die.

**Where it crashed** is captured too. Exec does trap handling in
supervisor mode and pushes a longword exception number below the CPU's
own frame, so the hook copies the supervisor stack verbatim and the
frame is decoded later, in daemon context, by matching Exec's exception
number against the CPU's vector offset — two independent fields agreeing
rather than one plausible-looking longword. SR and PC sit at the same
offsets on every 68k from the 68000 up; the fault address does not, and
is read for the six-word (`$2`), eight-word (`$4`, 68060) and 30-word
(`$7`, 68040) frames. **Dormant on Emu68**, which does not deliver CPU
exceptions to the guest, so no frame arrives to decode.

The patch runs in the crashing context, which may own almost nothing: it
copies the code and task name into globals, `Signal()`s the daemon (the
one exec call documented interrupt-callable), and chains to the original
so the alert still shows on the Amiga itself. Teardown follows the same
rule as the other patches — the vector is only restored if it still
points at ours.

## The snoop stream

`wasabi snoop` is the SnoopDOS trick delivered over the wire: the daemon
`SetFunction()`s thirty of the `dos.library` and `exec.library` calls a
developer most wants to watch — `Open`, `Lock`, `LoadSeg`, `Execute`,
`SystemTagList`, `GetVar`, `SetVar`, `DeleteFile`, `Rename`, `CreateDir`,
`MakeLink`, `CurrentDir`, `SetComment`, `SetProtection`, `RunCommand`,
`NewLoadSeg`, `DeleteVar`, `OpenLibrary`, `OpenDevice`, `FindPort`,
`FindTask`, `FindResident`, `FindSemaphore`, `OpenResource`, plus
icon.library's `GetDiskObject`, `GetDiskObjectNew`, `PutDiskObject`,
`FindToolType`, `MatchToolValue` and diskfont's `OpenDiskFont` — and
logs each call with its caller, its string/numeric arguments and its
real result:

```
$ wasabi snoop --task '#?'
Shell                Open("S:Startup-Sequence", read) = ok
Shell                Lock("DH0:Tools", read) = ok
cfile                LoadSeg("C:List") = ok
wasabid              GetVar("wasabi.key") = ok
```

A failed call's bare IoErr code is dressed by the client throughout
the stream — on screen and in the `--log` file alike, `= fail
(Error 205: Object not found)` rather than the wire's terse
`(err 205)` — and guru alert codes are decoded the same way. `run`'s
output is the one place left alone: there the failing command already
speaks for itself, and nothing is bolted on after it.

`--entry` logs each call on the way **in** as well as on the way out:

```
SysInfo              OpenDevice("brcm-emmc.device", unit 1) ...
SysInfo              OpenDevice("brcm-emmc.device", unit 1) = ok
```

A line with no partner is the call the machine died inside — the one
thing a log-on-return trace can never show, because the return never
happens. It costs a task switch per patched call (the daemon is
signalled immediately, to get the line on the wire before a freeze can
swallow it), so it is opt-in and deliberately slow.

`--output` sets how much survives to the screen and the log:

| mode | what it does |
|---|---|
| `full` | every line as it arrives, raw bytes and all — nothing folded, nothing hidden |
| `normal` (default) | folds a run of identical lines into one plus `... N more identical` |
| `minimal` | also hides routine noise: the five variables the shell probes before every command, `FindTask`, the Directory Opus port probe, and `OpenLibrary("dos.library")` |

The noise is real and it is relentless: bsdsocket's `WaitSelect` opens
`dos.library` once per daemon poll, which is twenty lines a second on
its own. Replaying a real 2306-line capture gives 600 lines at
`normal` and 196 at `minimal` — one run in it was **588 identical
lines**, folded to two. The opening line of every stream says which
mode is in force, so a log always describes what it left out; and a
pending fold is flushed when the stream falls quiet or drops, so a
freeze cannot swallow the tally of what the machine was doing.

`debug`, `snoop` and `debug --with-snoop` take the same five options —
`--task`, `--entry`, `--output`, `--log` and `--once`. The first two
steer the snoop trace, so a plain `debug` refuses them rather than
accept a flag it will not act on.

`--ignore-wasabi` hides the tool's own footprint: the daemon, the runner
it spawns behind `wasabi run`, an update's sidecar, and the temp file
output streams through. On a real 2601-line capture that leaves 609
lines, and 125 with `--output minimal` as well — the rest was wasabi
watching itself. What it never hides is anything the daemon says on
purpose: the guru report, the lost-frame counts, the farewell.

`--task PAT` filters on the caller's name with AmigaDOS wildcards
(`#?`, `?`, `*`); no filter means everything. The caller is named the way
SnoopDOS names it — the CLI command being run if there is one, else the
task name.

icon.library and diskfont.library are opened by the daemon for exactly
as long as they are patched — not politeness but necessity: a library
expunged with our stub in its jump table would take the stub with it.
Every LVO and register assignment comes from the NDK's `.fd` files, and
`OpenDiskFont`'s argument is a `TextAttr`, so that patch follows
`ta_Name` to print a font name rather than a pointer.

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

The event ring holds 512 entries (~107 KB), because a PiStorm-fast CPU
can fire more than 150 patched calls between two 50 ms drains. A burst
that overflows it is dropped and counted, never silently lost.

For the full picture, `wasabi debug --with-snoop` merges both streams
into one terminal over a single connection — the daemon tags every `LOG`
frame with its stream, so each line arrives prefixed `debug |` or
`snoop |`. And `--log FILE` on either command appends every line to a
file stamped with the client's receive time to the millisecond, which is
what lets output captured in parallel terminals be lined up afterwards:

```
2026-08-12 08:11:53.257 snoop | c:wasabid  Open("T:wasabi-run-2", readwrite) = ok
```

## Stack headroom

`wasabi ps` reports how much stack each task has **left**, not just how
much it was given:

```
ADDR       KIND  PRI STATE    STACK    FREE  CLI  NAME
0x08298d08 proc    1 run       8192    6104    5  Background CLI (c:wasabid)
0x092633d0 proc    1 wait    131072  130216    9  Background CLI (C:Wait)
0x08032400 task    5 wait      6144       -    -  input.device
```

Capacity says what a task was promised; headroom is the number that
predicts a crash. A task running out is called out on stderr rather than
left to be spotted in a column, and the threshold is a proportion — under
15% left, with an absolute floor for the very small. A byte count would
be wrong at both ends: 300 free of 512 is a healthy device task, 300 free
of 131072 is about to take the machine down.

`-` means the daemon declined to guess:

- A CLI program may swap stacks, leaving `sp` outside its own `Task`
  bounds — not an error, and not a measurement either.
- `tc_SPReg` is only meaningful for a task the scheduler has parked; for
  the running task it is stale by definition. The daemon samples its own
  stack pointer directly for that one entry, so it never prints `-` for
  itself.

The field is requested rather than always sent — see `PROTOCOL.md`. The
`PS` line had no room to grow, so adding a column unconditionally would
have shifted `<name>` for every client already deployed.
