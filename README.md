# Wasabi

Drive a real Amiga from a Linux box. Upload a binary anywhere on the
system, run it and watch its output arrive live, and reboot the machine —
from the same terminal the code was written in.

Two halves:

- **`wasabid`** — a small daemon on the Amiga (C, Bebbo's gcc, one file).
- **`wasabi`** — the Linux client (Python 3, stdlib only, one file).

```
$ wasabi discover
192.168.1.42    :1234  amiga        wasabid 0.1

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
| `reboot` | built, **never fired in anger** |
| `debug` (KPrintF stream) | **not implemented** — returns a clear error |
| `snoop` (DOS call trace) | **not implemented** — returns a clear error |

First live run: 12 August 2026, against an A1200 + PiStorm32-lite/CM4 on
Emu68 with the lwIP `bsdsocket.library`. Kickstart 47.115, 3.4 ms
round trip, a 19372-byte binary round-tripped byte-identical, `List
SYS:C` streamed all 119 entries, and a failing command reported
`rc 10, IoErr 205` correctly.

The client is additionally exercised end to end by `make test` against a
host mock that speaks the same protocol — 15 tests, no Amiga required.

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
the Emu68 driver stack's lwIP stack over `genet.device`, or Roadshow, or
AmiTCP. Anything that provides the standard API.

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
wasabi info                  Kickstart version, free chip/fast RAM
wasabi ls [PATH]             list a drawer; no path lists the volumes
wasabi put LOCAL REMOTE      upload a file
wasabi get REMOTE LOCAL      download a file ('-' for stdout)
wasabi run "CMD"             execute, stream output, return its exit code
wasabi deploy L R [--run C] [--reboot]
wasabi del PATH / mkdir PATH
wasabi reboot [--cold]
wasabi debug                 live KPrintF stream          (not yet)
wasabi snoop [--task PAT]    live DOS call trace          (not yet)
```

`--host` overrides discovery, `WASABI_HOST`/`WASABI_KEY` override the
config file.

## Three things the Amiga side gets right on purpose

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

## Security

`wasabid` executes arbitrary commands as whatever started it, over a
plaintext socket, on a machine with no memory protection. The key in
`ENV:wasabi.key` stops an accidental connection from a misconfigured
tool; it stops nothing else. Do not port-forward it. The discovery
responder answers everyone, deliberately — needing the key to *find* the
machine would defeat the point, and it reveals no more than a port scan.

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

## What's next

`debug` and `snoop` are the reason this is a daemon and not an FTP
server, and they are the half that is not written yet. Both work the
same way: `SetFunction()` patches on library vectors, output into a ring
buffer, drained to the socket.

- **`debug`** patches exec's `RawPutChar` — one vector, the Sashimi
  trick. The patch runs in interrupt and Supervisor context, so it must
  allocate nothing, take no locks, and do no I/O: copy bytes into a
  preallocated ring, `Signal()` the daemon, done.
- **`snoop`** patches a dozen `dos.library` entry points — the SnoopDOS
  trick. These run in ordinary task context and may format text, but
  must never call a function they have themselves patched.

Both must unpatch in exact reverse order and only after checking the
vector still points at theirs. If anything has patched over the daemon,
it must refuse to quit and say so — see PROTOCOL.md.

`PROTOCOL.md` is the wire format, and the contract between the two halves.
