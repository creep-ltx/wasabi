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
contain spaces (it runs to the end of the line). `name` may not.

The client broadcasts, waits ~500 ms, and collects every reply. One
answer and it connects; several and it prints the list and asks which.
The winner is cached in `~/.cache/wasabi/last-host` so the next command
skips the probe, falling back to a fresh probe if the cached address
stops answering. An explicit `--host` always wins and never probes.

`wasabid` answers probes unconditionally — an unauthenticated reply
reveals only what a port scan would, and needing the key to *find* the
machine would defeat the point. The key still gates every TCP session.

There is deliberately no periodic beacon by default: a machine that
shouts every 30 seconds is noise on a network that may be shared, and
probe/response gets the same result at the moment it is actually wanted.
`BEACON=<secs>` in the config turns one on for setups where the client
cannot broadcast.

Full mDNS is the obvious future upgrade — the Emu68 driver stack already
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

Strings are **not** NUL-terminated on the wire. Where a payload holds a
string it is `u16 len` followed by `len` bytes, Latin-1 (the Amiga's
charset). The C side always terminates them itself after bounds-checking.

## Tags

| Tag  | Name     | Direction | Payload |
|------|----------|-----------|---------|
| 0x01 | HELLO    | C→S | `u16 version`, `str key` |
| 0x02 | WELCOME  | S→C | `u16 version`, `str banner` |
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
| 0x30 | DEBUG    | C→S | `u32 flags` |
| 0x31 | SNOOP    | C→S | `u32 flags`, `str taskfilter` |
| 0x32 | LOG      | S→C | `u32 stream`, `u32 seq`, `str text` |
| 0x40 | REBOOT   | C→S | `u32 flags` |
| 0x41 | INFO     | C→S | — |
| 0x42 | RESTART  | C→S | — |

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

The server runs the command through `SystemTagList()` with its input and
output redirected, and forwards whatever appears as `STDOUT`/`STDERR`
frames as it appears. When the command finishes it sends `EXIT` carrying
the return code and `IoErr()`.

**Liveness is bounded by the child's own buffering**, exactly as on Unix:
a C program writing through buffered stdio to something that is not a
console will emit in block-sized lumps no matter what the daemon does.
AmigaDOS commands that write with `Write()` stream immediately.

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

The daemon patches `RawPutChar()` in exec's vector table with
`SetFunction()` and appends to a ring buffer. The patch runs in whatever
context the caller was in — **including interrupts and Supervisor mode** —
so it allocates nothing, takes no locks, and does no I/O; it copies bytes
into a preallocated ring and `Signal()`s the daemon.

If the ring overflows because nobody drained it fast enough, the daemon
emits a `LOG` line saying how many bytes were lost rather than silently
dropping them.

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

`flags` bit 0 requests a cold reboot. The daemon replies `OK`, flushes
every mounted volume, waits for the socket to drain, then calls
`ColdReboot()`. The connection dies with the machine; the client treats a
close after `OK` as success.

### RESTART

Reloads the daemon in place — the fast half of self-update. `put` over a
running `C:wasabid` succeeds because `LoadSeg` copies the binary into
memory and does not lock the file, so `put` then `RESTART` swaps in a new
build without a reboot. The daemon replies `OK`, then exits; on the way
out — *after* it has closed the listen socket, so the fresh instance can
bind the same port — it relaunches itself via `GetProgramName()` (the
path it was invoked by) on the same port. The client treats a close after
`OK` as success and reconnects.

## Teardown and the SetFunction rule

Patches are removed in exact reverse order, and only after checking that
the vector still points at ours. **If anything patched over us, `wasabid`
refuses to quit** and says so — removing a patch that someone else has
chained onto is how you crash a machine ten minutes later, in a place
that looks nothing like the cause. A daemon that cannot exit cleanly is a
far smaller problem than a Guru with no traceable origin.
