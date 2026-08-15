# Wasabi wire protocol

Version 1. One TCP connection carries one command. Streams (`DEBUG`,
`SNOOP`) hold the connection open until the client hangs up; one
connection may subscribe to both at once — every `LOG` frame names its
stream, and each stream numbers its frames independently.

All integers are **big-endian** — the m68k's natural order, so the Amiga
side needs no byte swapping anywhere.

## Discovery

The client should not need to be told an IP address. `wasabid` listens on
**UDP port 1234** — the same number as its TCP port — and answers a
broadcast probe:

```
client -> 255.255.255.255:1234   WASABI?1\n
server -> (unicast back)         WASABI!1 <name> <tcpport> <banner>\n
```

Fields are space-separated, the line ends with `\n`, and `banner` may
contain spaces (it runs to the end of the line). `name` may not: the
daemon sends its `name <id>` startup argument if one was given, else
`ENV:HOSTNAME`, spaces turned to dashes either way — or `amiga` when
neither is set.

The client broadcasts, waits ~500 ms, and collects every reply. One
answer and it connects; several and it prints the list and asks which.
The winner is cached in `~/.cache/wasabi/last-host` so the next command
skips the probe, falling back to a fresh probe if the cached address
stops answering. An explicit `--host` always wins and never probes.

`wasabid` answers probes from the LAN without asking for the key — an
unauthenticated reply reveals only what a port scan would, and needing
the key to *find* the machine would defeat the point. The key still
gates every TCP session. It stays silent to any address it would refuse
a connection from; see **Who may connect** below.

There is deliberately no periodic beacon: a machine that shouts every 30
seconds is noise on a network that may be shared, and probe/response gets
the same result at the moment it is actually wanted — the client's
unicast sweep covers the bridge that eats broadcasts. A `BEACON=<secs>`
option remains a possible future addition for a network where even the
sweep cannot reach the machine.

Full mDNS is the obvious future upgrade — rondoval's driver stack already
ships an `mdns` tool, so `amiga.local` may resolve on this network
without Wasabi's help. Wasabi does not depend on it.

## Framing

Every message in both directions is one frame:

```
u8   tag
u32  length
u8   payload[length]
```

`length` may be zero. Maximum payload is 65536 bytes; a peer that sees a
larger length must close the connection rather than try to allocate it.
That cap is the only thing standing between a stray byte and a 4GB
`AllocMem()` on a machine with no MMU protection.

**Send a frame whole.** The daemon gives a peer ten seconds to finish
what it started — on either direction, since a client that stops reading
a stream blocks the daemon's `send()` just as surely — and then drops the
connection. It is one process with one loop, so an unbounded wait is the
whole machine's wait. Nothing legitimate comes close: a full 64 KB frame
crosses a LAN in well under a millisecond.

Strings are **not** NUL-terminated on the wire. Where a payload holds a
string it is `u16 len` followed by `len` bytes, Latin-1 (the Amiga's
charset). The C side always terminates them itself after bounds-checking.

## Tags

| Tag  | Name     | Direction | Payload |
|------|----------|-----------|---------|
| 0x01 | HELLO    | C→S | `u16 version`, `str key` |
| 0x02 | WELCOME  | S→C | `u16 version`, `str banner`, `str caps`, `u32 refused` |
| 0x03 | ERR      | S→C | `u32 code`, `str message` |
| 0x04 | OK       | S→C | — |
| 0x05 | PING     | C→S | — |
| 0x06 | PONG     | S→C | — |
| 0x10 | PUT      | C→S | `u32 size`, `u32 protbits`, `str path` |
| 0x11 | GET      | C→S | `str path` |
| 0x12 | DATA     | both | raw bytes |
| 0x13 | END      | both | — |
| 0x14 | LS       | C→S | `str path` |
| 0x15 | DEL      | C→S | `str path` |
| 0x16 | MKDIR    | C→S | `str path` |
| 0x20 | RUN      | C→S | `u32 flags`, `str command` |
| 0x21 | STDOUT   | S→C | raw bytes |
| 0x22 | STDERR   | S→C | raw bytes |
| 0x23 | EXIT     | S→C | `u32 rc`, `u32 secondary` |
| 0x30 | DEBUG    | C→S | `u32 flags` (reserved; the daemon does not read it) |
| 0x31 | SNOOP    | C→S | `u32 flags`, `str taskfilter` |
| 0x32 | LOG      | S→C | `u32 stream`, `u32 seq`, `str text` |
| 0x40 | REBOOT   | C→S | `u32 flags` |
| 0x41 | INFO     | C→S | — |
| 0x42 | RESTART  | C→S | `[u32 flags]` (may be absent) |
| 0x43 | PS       | C→S | `[u32 flags]` (may be absent) |
| 0x44 | KILL     | C→S | `u32 flags`, `str target` |
| 0x45 | SPEED    | C→S | `u32 flags`, `u32 size`, `str target` |
| 0x46 | QUIT     | C→S | `[u32 flags]` (may be absent) |
| 0x47 | INSTALL  | C→S | `str sidecar` |
| 0x48 | GRAB     | C→S | `str screenname` |
| 0x49 | SCREEN   | C→S | `u32 flags`, `str title` |

`str` = `u16 len` + bytes, as above.

## Handshake

The client opens the connection and sends `HELLO` immediately. The server
answers `WELCOME` or `ERR` and, on `ERR`, closes.

The key is a shared secret read from `ENV:wasabi.key` on the Amiga. It is
compared as raw bytes. **This is a plaintext credential on an unencrypted
socket** — it exists to stop an accidental connection from a misconfigured
tool on the LAN, not to withstand an attacker who can see your traffic.
`wasabid` is a remote-code-execution daemon by design and belongs on a
trusted network only.

Version mismatch is fatal in both directions: the daemon will not try to
speak v1 to a v2 client.

`SNOOP`'s leading u32 is a flags word: bit 0 (`snoopentry`) asks the
daemon to log each patched call on the way in as well as on the way
out, so a call that never returns still appears. The daemon advertises
`snoopentry` in caps when it understands the bit; older daemons ignore
the word entirely, which is why the client checks caps rather than
assuming silence means yes.

`caps` is a comma-separated list of what the daemon can actually do —
`ping,info,ls,put,get,run,del,mkdir,debug,snoop,reboot,restart,ps,kill,`
`speed,speedfile,quit,install,grab,screen,hb,guru,snoopentry,psfree` for
a current build. Self-update makes version skew
an everyday event: the client is usually a `git pull` ahead of the daemon
until the next `wasabi update`, and "unknown command" is a poor way to
find that out. With the list, the client can name the build that is too
old and say what to do about it.

**Adding it broke nothing in either direction, by construction.** Frame
lengths are explicit and a client reads the banner as a counted string,
so one that predates `caps` stops before it and never notices. A client
that expects `caps` and finds the payload ended treats that as *unknown*,
never as *supports nothing*, and falls back to sending the command and
reporting whatever comes back. Both paths were checked against the real
A1200: a current client against a daemon with no list, and a client from
before the list against a daemon that sends one.

`PROTO_VERSION` remains the hard gate, and only for framing changes.
Features negotiate through `caps`.

`refused` is how many connections the daemon has turned away as not
being on the LAN, counted since the tally file was last cleared. It is
appended after `caps` on the same compatibility argument, and exists so
the operator learns that something has been knocking without having to
go looking for it.

## Who may connect

The daemon closes any connection from an address it does not recognise
as local — RFC1918, loopback and link-local by default — **before** the
peer can send `HELLO`, and does not answer that address's discovery
probes either. Extra ranges can be permitted at startup (`allow <cidr>`,
for a mesh VPN) or the check disabled entirely (`allow any`), which
warns loudly because `wasabid` is a remote-code-execution daemon.

This is a guard against exposure by accident — a forwarded port, UPnP, a
DMZ — not against anyone already on the LAN. Spoofing does not defeat
it for TCP: an off-path attacker forging a private source address never
receives the SYN-ACK and cannot complete the handshake.

## Commands

### PUT — write a file anywhere

Client sends `PUT` with the final size and the Amiga path, then one or
more `DATA` frames totalling exactly `size`, then `END`. Server replies
`OK` or `ERR`.

`protbits` is the AmigaDOS protection mask to apply with `SetProtection()`
after the close, or `0xFFFFFFFF` to leave the default alone. The bit sense
is AmigaDOS's own — the low four bits (`d`, `e`, `w`, `r`) are *active
low*, so a plain executable is `0` and the client must not "helpfully"
send Unix mode bits.

The server writes to a temporary name in the same directory and renames
over the target on `END`, so an interrupted transfer never leaves a
half-written binary where a working one used to be. On a failed transfer
the temporary is deleted.

### GET — read a file

Server replies with `DATA` frames then `END`, or a single `ERR`.

### RUN — execute, with live output

`flags`:

| Bit | Meaning |
|-----|---------|
| 0   | merge stderr into stdout |
| 1   | do not wait — detach and reply `EXIT 0` at once |

**Today's `wasabid` ignores both flags**: output arrives merged on
`STDOUT` (one redirected file carries everything `SystemTagList()`
produces) and the daemon always waits for the command. The flags are
reserved wire protocol, honoured by the host mock; a daemon that grows
real stderr separation or detach will find clients already sending them.
To run something without waiting, use the shell's own detach:
`wasabi run "Run <command>"` returns as soon as `Run` has spawned it.

The server runs the command through `SystemTagList()` with its input and
output redirected, and forwards whatever appears as `STDOUT`/`STDERR`
frames as it appears. When the command finishes it sends `EXIT` carrying
the return code and `IoErr()`.

**Liveness is bounded by the child's own buffering**, exactly as on Unix:
a C program writing through buffered stdio to something that is not a
console will emit in block-sized lumps no matter what the daemon does.
AmigaDOS commands that write with `Write()` stream immediately.

**The tail sees through ram-handler's blind spot.** A ram-handler file
handle anchors to the file's data at `Open()` time: opened while the
file is still empty, it never sees bytes another handle appends later —
not after `Flush()`, not after the writer closes, not after `Seek()`.
The daemon's tail handle opens moments after the runner starts, so a
command whose first byte arrived later than that — anything that slept
first, or flushed buffered stdio only at exit — used to come back with
its entire output missing, while fast starters worked and made the
failures look sleep-related. The daemon detects the stale handle with
`ExamineFH()` (which does report the true size) and reopens, resuming
at the bytes already forwarded. `tests/latewrite.c` is the probe;
`tests/slowwrite.c` proves the opposite property, live tailing.

### LS — directory listing

Server replies with `DATA` frames holding one entry per line:

```
<type> <size> <protbits> <days> <mins> <ticks> <name>
```

`type` is `f` or `d`. The date is the raw `DateStamp` triple, left for the
client to format — the Amiga has no strftime and the client does.

### DEBUG — the KPrintF stream

Subscribes to raw serial-debug output: everything any program on the
machine writes with `KPrintF()`/`RawPutChar()`, which is where Amiga
software has always put the messages it cannot afford to route through
the display. This is what Sashimi captures, delivered over the wire
instead of into a file.

The leading `flags` word is reserved: the daemon accepts the frame and
never reads it. It exists so this subscription can grow options the way
`SNOOP`'s did, without a framing change. A client should send zero.

The daemon patches `RawPutChar()` in exec's vector table with
`SetFunction()` and appends to a ring buffer. The patch runs in whatever
context the caller was in — **including interrupts and Supervisor mode** —
so it allocates nothing, takes no locks, and does no I/O; it copies bytes
into a preallocated ring and `Signal()`s the daemon.

If the ring overflows because nobody drained it fast enough, the daemon
emits a `LOG` line saying how many bytes were lost rather than silently
dropping them.

**Heartbeat** (`hb` in caps). While any stream is subscribed the daemon
sends an empty `LOG` — zero-length text, sequence numbered as usual —
on it every ~5 seconds. The client renders nothing; the point is that
silence becomes information. A reset or frozen machine sends no FIN, so
without this a stream client would block in `recv()` forever, unable to
tell a quiet machine from a dead one. A client that misses heartbeats
should *warn*, not exit: the daemon is single-threaded, and a large
transfer on another connection legitimately starves heartbeats for as
long as it runs. The daemon in turn treats a heartbeat as a courtesy,
not a delivery: if the subscriber's socket cannot take it without
waiting — a sleeping laptop, a blinking Wi-Fi hop — the beat is
skipped, not blocked on. Only a send that actually *fails* frees the
stream slot; a slow one never does.

**Guru report** (`guru` in caps). The daemon patches exec's `Alert()`
(LVO -108) for its whole lifetime. When any task on the machine raises
an alert, one visible line goes out on every subscribed stream:

```
[wasabi: DEADEND ALERT #81000005 in task 'c:wasabid' (0x08298d08)]
```

**Scope.** This sees alerts raised through the `Alert()` vector —
exec's own (`AN_MemCorrupt` and friends), and programs that call
`Alert()` deliberately. It does **not** see a program faulting: a CPU
exception goes to the task's `tc_TrapCode`, which for a Process is
dos.library's "Software Failure" requester, and never touches
`Alert()`.

(or `RECOVERABLE` for alerts without the deadend bit). The line escapes
before the guru freezes the display whenever the alert is raised in
task context — the daemon runs at priority 1 and is signalled from the
patch, so it preempts the dying task for the milliseconds the send
takes. An alert raised in supervisor mode or an interrupt cannot be
escaped live. For those the daemon keeps a black box: a deadend alert
is stashed by the patch itself — magic words, code, task name,
checksum, and a snapshot of the supervisor stack — just under
`mh_Upper` of the highest fast-RAM MemHeader, in a 512-byte region the
daemon `AllocAbs()`es at startup and holds for its whole life. Owning it is the point: free memory on this machine is
where exec keeps its free list, so writing into a chunk the daemon did
not own would corrupt that list and guru the machine with
`AN_MemCorrupt` at some unrelated `FreeMem` later. The daemon's
next life checks that spot (and `ExecBase->LastAlert`, which real
hardware preserves across a warm reboot; Emu68 does not — poke it and
reboot, it returns FFFFFFFF) and picks the report up from there.

Every captured alert — live or found in `LastAlert` at startup — also
becomes one line of plain text in `T:lastguru`, stamped with the date
and time it was seen. `T:` is RAM-backed: the note survives daemon
restarts and self-updates, and dies with the boot — exactly when the
black box takes over as the surviving copy. (`LastAlert` would serve
that role on hardware that preserves it across a reset; Emu68 does
not.) On the Amiga itself, `Type T:lastguru` reads the same note.

Nothing else in RAM keeps a copy. A guru cannot be recovered after the
fact — it can only be recorded in advance, somewhere the boot will not
reuse.

Every new stream subscription replays the note as its first line:

```
[wasabi: last guru: DEADEND ALERT #81000005 (exec: corrupt memory list detected in FreeMem) in task 'c:wasabid' (0x08298d08) - 13-Aug-26 19:27:12]
```

(a combined debug+snoop attach sees it once, not twice). A client that
reconnects its streams automatically therefore closes the loop on its
own: the machine gurus, reboots, the daemon comes back, the stream
resubscribes, and the alert code arrives in the same terminal that
watched the machine die. All these lines ride ordinary `LOG` frames —
there is nothing new on the wire, and `guru` in caps only states that
this daemon produces them.

**Trial instances.** `update` proves a new binary can serve by running
it on a spare port before committing to it — which means two daemons
alive at once, and one of them about to unload. A daemon started with
the `trial` argument therefore patches nothing and claims no shared
memory: no `Alert()` hook, no black-box region, and `DEBUG`/`SNOOP`
are refused. Without it, the trial instance's `SetFunction` patches
outlive its code segment, and the machine gurus with a privilege
violation (`8000 0008`) the next time anything calls through the stale
jump-table entry — which is exactly what it did, reproducibly, until
this was added.

**Farewell.** A daemon going down on purpose — `REBOOT`, `RESTART`,
`QUIT`, or Break at its console — writes one last visible line on each
open stream, in the same voice as its connect notices:

```
[wasabi: rebooting - closing this stream]
```

(or `restarting` / `stopping`), then closes every client socket
properly. A deliberate exit is therefore always distinguishable from a
crash: the goodbye names the reason, the FIN ends the stream cleanly,
and a stream that just stops with neither is a machine in trouble.

### SNOOP — the DOS call trace

The SnoopDOS trick: `SetFunction()` patches on `dos.library` and
`exec.library` entry points, each logging caller, arguments and result.
Covered calls are `Open`, `Lock`, `LoadSeg`, `Execute`, `SystemTagList`,
`GetVar`, `SetVar`, `DeleteFile`, `Rename`, `CreateDir`, `MakeLink`,
`OpenLibrary`, `OpenDevice` and `FindPort`.

`taskfilter` restricts output to tasks whose name matches an AmigaDOS
pattern (`#?`, `?`, `*`, case-insensitive); empty means everything. The
filter is applied daemon-side against the caller's name — the CLI command
being run if the caller is a CLI running one, else the task's `ln_Name`.

Snoop output rides `LOG` frames with `stream` set to **1**, distinct from
the debug stream's **0**, so a client subscribed to both can keep them
apart. Each line reads `<task> <Call>(<args>) = <result>`.

`SNOOP` self-tests before it installs anything: the daemon `Lock()`s a
bogus path of its own and checks the captured event carries back that
exact string, mode and result through the trampoline's register file. A
mismatch means the asm and that build disagree, and the daemon answers
`ERR` instead of snooping — see the teardown note below for why a patch
that misreads its arguments is worse than no patch at all.

Unlike the `RawPutChar` patch these run in ordinary task context, so they
may format text — but the trampoline still runs in the *caller's* context
and must never call a function it has itself patched, on pain of unbounded
recursion: the daemon-side record reads `SysBase->ThisTask` directly and
only copies memory. The log line is emitted *after* the original returns,
so it carries the real return value and `IoErr()`. Two safeguards follow
SnoopDOS: a per-stub enabled flag lets a patch that cannot be removed idle
as a near-no-op, and an event is dropped (and counted) rather than built
when the caller is low on stack. Covered LVOs, verified against the NDK
`fd` files: `Open` -30, `DeleteFile` -72, `Rename` -78, `Lock` -84,
`CreateDir` -120, `LoadSeg` -150, `Execute` -222, `MakeLink` -444,
`SystemTagList` -606, `SetVar` -900, `GetVar` -906 on `dos.library`;
`FindPort` -390, `OpenDevice` -444, `OpenLibrary` -552 on `exec.library`.

### REBOOT

The daemon replies `OK`, says goodbye on any open stream (see *Farewell*
under DEBUG), closes **every** client socket — a reset machine sends no
FIN, so a connection left open here would wait forever on a peer that no
longer exists — asks every mounted volume's handler to write out its
dirty buffers (`ACTION_FLUSH`), gives it all a moment to drain, then
calls `ColdReboot()`. The client treats a close after `OK` as success.

`flags` bit 0 (cold) is accepted and ignored: `ColdReboot()` is the only
reset exec sanctions a program to make, so every reboot is cold. The bit
stays reserved for a build that finds a genuinely warm path.

### RESTART

Reloads the daemon in place — the fast half of self-update. `put` over a
running `C:wasabid` succeeds because `LoadSeg` copies the binary into
memory and does not lock the file, so `put` then `RESTART` swaps in a new
build without a reboot. The daemon replies `OK`, then exits; on the way
out — *after* it has closed the listen socket, so the fresh instance can
bind the same port — it relaunches itself via `GetProgramName()` (the
path it was invoked by) on the same port. The client treats a close after
`OK` as success and reconnects.

Both `RESTART` and `QUIT` answer `ERR` while a command is running: the
runner executes the daemon's own code segment, which the shell unloads
the moment the daemon exits — a Guru minutes later, nowhere near the
cause. Wait for the command or `KILL` it first. `REBOOT` is exempt;
the machine dies anyway, and the runner with it.

Both take an optional `u32 flags`; bit 0 (`--force`) asks the daemon to
Ctrl-C the running command's process first and wait up to ten seconds
for it to die — `SystemTagList()` hands back no child, so the process
is found by the command name its CLI is executing, the way an operator
would find it. A command that ignores the signal still gets `ERR`: the
force is polite on purpose, because `RemTask()` on the child would
leave the runner waiting forever on a death packet that never comes. A
daemon from before this paragraph ignores the payload — and has no
guard to override.

### QUIT — stop, and stay stopped

Replies `OK`, then exits without relaunching. This is how the throwaway
instance that `wasabi update` starts on a spare port is shut down once it
has proved itself. Aimed at the *main* daemon it is a one-way door:
nothing brings it back but physical access, which is why the client
demands `--yes`.

### INSTALL — swap a verified sidecar in for the running binary

`PUT` **refuses** the path the daemon is running from — compared with
`SameLock()`, so aliases like `SYS:C/wasabid` are caught too. Otherwise
any file at all could become the daemon and the next restart would take
the machine off the network, with no way back but physical access.

`INSTALL` is the one route to that path. The daemon renames its own
binary to `<self>.bak` and the named sidecar into its place, and replies
`OK`. Renames only: the bytes that passed verification are exactly the
bytes installed, and the previous binary stays one rename away. If the
second rename fails the first is undone, so a failed install leaves a
working daemon rather than a machine with no binary at all.

The verification that earns an `INSTALL` is entirely the client's, and
needs no wire support of its own — `PUT` to a sidecar, `RUN` to check
identity (`Version <sidecar> FULL`) and then behaviour (`<sidecar>
--selftest <nonce>`, which must echo a marker carrying that nonce back,
since exit status alone proves nothing), `RUN` again to start it on a
spare port for a real handshake, `QUIT` to stop it, then `INSTALL` and
`RESTART`.

### PS — list every task on the machine

A snapshot of exec's scheduler state: `ThisTask` plus the `TaskReady` and
`TaskWait` lists, walked under `Disable()` with everything copied out —
a task pointer is only trustworthy while interrupts are off. Server
replies with `DATA` frames, one task per line, then `END`:

```
<addr> <kind> <pri> <state> <stack> <cli> <name>\t<cmd>
```

`addr` is `0x`-hex (the task's address — its only stable identity),
`kind` is `p`rocess or `t`ask, `state` is `run`/`ready`/`wait`, `cli` is
the CLI number or `-1`, and `cmd` — after the tab, since both names may
hold spaces — is the CLI command the process is running, empty otherwise.
Filtering is the client's job.

**`flags` bit 0 (`psfree`)** asks for a headroom field, inserted after
`<stack>`:

```
<addr> <kind> <pri> <state> <stack> <free> <cli> <name>\t<cmd>
```

`stack` is the task's capacity (`tc_SPUpper - tc_SPLower`); `free` is how
much of it is left. `-1` means the daemon declined to guess — either the
task swapped stacks, so its `sp` is outside its own `Task` bounds, or the
value would have been read from a `tc_SPReg` that is stale. (For the
running task the daemon samples its own stack pointer directly, since
`tc_SPReg` is by definition out of date for whoever is executing.)

The field is **requested, not simply added**, and that is deliberate: the
line has no room to grow. Inserting a field unconditionally would shift
`<name>` for every client already deployed, and appending after `<cmd>`
is impossible because `<cmd>` is the tab-delimited remainder. A daemon
that predates the flags word never reads the payload, so an old client
(which sends none) and a new client talking to an old daemon both get the
seven-field line they expect.

### KILL — stop a task

`flags` bit 0 clear: `Signal()` the target `SIGBREAKF_CTRL_C` — exactly
what the `Break` command does, and only as effective as the target's
willingness to listen. Bit 0 set: `RemTask()` it outright, which releases
none of the locks, semaphores or DOS state the task holds — a last
resort, flagged accordingly (`--force`).

`target` is either a name — matched case-insensitively against both the
task name and the running CLI command — or a `0x` address from `PS`. It
must match exactly one task: no matches, several matches, and the daemon
itself are each a distinct `ERR`. The action happens under `Disable()`
after re-finding the task in the scheduler lists, so a target that
exited after `PS` is reported gone, never a stale pointer.

### SPEED — storage-free throughput measurement

`flags` bit 0 picks the direction. Clear: the client follows with `DATA`
frames totalling exactly `size` bytes, then `END`; the server counts and
**discards** every byte and replies `OK`, or `ERR` on a size mismatch.
Set: the server sends `DATA` frames totalling `size` bytes from a static
pattern buffer, then `END`.

With `target` empty nothing touches RAM: or any volume in either
direction — the test cannot fill a machine up, and the number isolates
the network path.

A non-empty `target` writes through that volume instead, so the same
command measures the filesystem. The daemon `Info()`s the volume first
and refuses unless the size plus 8 MB of margin fits, since a full disk
is a worse outcome than a missing measurement; it also refuses a
write-protected volume. The sink half writes `wasabi-speed.tmp` there,
the source half reads it back and deletes it. Advertised as `speedfile`
in `caps`, because a daemon that predates it would silently ignore the
target and report network speed as though it were disk speed. `size` must
be 1 byte to 256 MB; anything else is an `ERR`. Timing is entirely the
client's business.

### GRAB — a screen as raw pixels

Server replies with `DATA` frames then `END`. The first twelve bytes are
`u32 width`, `u32 height`, `u32 bytes-per-pixel` (3, meaning RGB), and
the rest is `width * height * 3` bytes of pixels, top row first.

**Uncompressed on purpose.** The network is the fastest component in
this system by a wide margin, so shipping 3.7 MB raw costs about thirty
milliseconds of wire while compressing it on the Amiga would cost
seconds. The client makes the PNG.

`screenname` empty means the **frontmost** screen, private ones
included; a name locks that public screen instead. How the pixels are
read is decided by the screen's depth: above 8 bits by CyberGraphX's
`ReadPixelArray()`, at or below by `graphics.library`'s
`ReadPixelArray8()` plus the screen's palette. The client is told
neither — the reply is identical.

The frontmost screen is read via `LockIBase()`, which makes the pointer
safe to take but does not hold the screen open; closing it during the
read is a genuine race. A named public screen is locked properly.

### SCREEN — list them, and reorder

`DATA` frames of `<addr> <width> <height> <depth> <title>`, front first,
then `END`. `flags` bit 0 sends the frontmost screen to the back — the
same thing Amiga+M does — and a non-empty `title` brings that screen to
the front instead. Both are `ScreenToBack()`/`ScreenToFront()`; no
keystrokes are synthesised into `input.device`.

## Teardown and the SetFunction rule

Patches are removed in exact reverse order, and only after checking that
the vector still points at ours. **If anything patched over us, `wasabid`
refuses to quit** and says so — removing a patch that someone else has
chained onto is how you crash a machine ten minutes later, in a place
that looks nothing like the cause. A daemon that cannot exit cleanly is a
far smaller problem than a Guru with no traceable origin.
