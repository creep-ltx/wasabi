# Installing and updating

[Back to the README](../README.md)

## Installing

**Amiga.** Download `wasabid` (any 68k) or `wasabid.020` (68020 or
better) from the [latest release](https://github.com/creep-ltx/wasabi/releases/latest),
or build it (see *Building and testing*). Copy it to `C:wasabid`, set a
key, and start it:

```
Copy wasabid.020 C:wasabid
SetEnv SAVE wasabi.key hunter2
Run >NIL: C:wasabid
```

Put those last two lines in `S:User-Startup` to have it up after every
boot. Stop it with `Break <n> C`.

It needs a working `bsdsocket.library` — on a PiStorm32/CM4 that means
[rondoval's emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack)
(lwIP over `genet.device`), or Roadshow, or AmiTCP. Anything that
provides the standard API.

**The PC.** Either a ready-made file from the release - nothing else
to install - or the Python script:

| | |
|---|---|
| Linux (x86-64, ARM64 / Raspberry Pi) | `wasabi-linux-x86_64`, `wasabi-linux-arm64` |
| macOS (Apple Silicon, Intel) | `wasabi-macos-arm64`, `wasabi-macos-x86_64` |
| Windows | `wasabi-windows-x64.exe` |
| anything with Python 3 | `wasabi` (the script; with this repository it also runs the desktop app's window and the NAS server from source) |

Rename the file to `wasabi` (`wasabi.exe` on Windows), make it
executable on Linux and macOS (`chmod +x wasabi`), and put it on your
path, e.g. `~/.local/bin`. The downloads are not signed: on macOS run
`xattr -d com.apple.quarantine wasabi` once; on Windows SmartScreen asks
- choose "More info", then "Run anyway". From a clone instead:
`ln -s $PWD/wasabi ~/.local/bin/wasabi`.

Then give it the key:

```
mkdir -p ~/.config/wasabi
printf 'key = hunter2\n' > ~/.config/wasabi/config
```

On Windows the same file is `%USERPROFILE%\.config\wasabi\config`. No
host is needed in the config - the client finds the Amiga by broadcast
(and on networks where broadcast does not cross from Wi-Fi to wired, by
a sweep of the local addresses) and caches the answer.

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
updating to wasabid 0.2
wasabid -> C:wasabid.new (44244 bytes)
  identity   wasabid 0.2 (2026-08-15)
  selftest   ok
  live       wasabid 0.2 served a handshake and a ping on port 1235
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

Check 4 is the one that earns its keep. A wasabid that binds its port
and then exits immediately is a genuine build with the right `$VER` and a
self-test that really does open `bsdsocket.library` — checks 1, 2 and 3
**all pass it**, and installing it would take the machine off the network
at the next restart. Only connecting to it catches that:

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
wasabid 0.2, protocol v1
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

## Building and testing

```
make            # cross-compile wasabid (any 68k)
make both       # and wasabid.020 (68020+)
make test       # client vs. the host mock - no Amiga needed
python3 tools/build-binary.py   # the client as one file (pip install pyinstaller)
```

`tests/mock-wasabid.py` serves the same protocol out of a host directory.
It is both the test fixture and the reference the C daemon was written
against: where the two disagree about the wire, the mock is what the
client was proven with.

The cross-compiler is Bebbo's `m68k-amigaos-gcc`, at
`$(HOME)/opt/amiga/bin` on this box. Override `CC` if yours lives
elsewhere. No CyberGraphX or Picasso96 developer headers are needed:
the few calls the screen grab uses are declared in
`include/wasabi_cgx.h` (since 0.9; 0.8 needed them, found by Spot). The
first `make` downloads the AmiSSL 5.27 developer kit
(`third_party/fetch.sh`, checked by its SHA-256) for `tls.c`; the
daemon only opens AmiSSL at run time, so it runs without it. It is
built with `-fbbb=-`: bebbo's own optimizer pass miscompiles loops over
small structs, and did so here.

`view/` is the desktop and phone app (React); its built `dist/` is in
git, so nothing needs Node unless you change it. `view/test/` drives
it in a headless Firefox; `docs-*.mjs` take the README's pictures.

**A release carries both halves, always.** `wasabid`, `wasabid.020`
and `wasabi` are attached to the same tag, and they are the pair that
was tested together. The one-file clients are added by GitHub Actions
(`.github/workflows/binaries.yml`) when the release is published: built
with PyInstaller on Linux (x86-64, ARM64), macOS (ARM, Intel) and
Windows - it does not cross-build - and each smoke-tested against the
mock before it is attached. Shipping a daemon on its own leaves whoever downloads it to
find a client from wherever, which is the version skew the capability
negotiation exists to survive rather than something to invite.
