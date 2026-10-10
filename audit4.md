# Wasabi — Fourth Audit (0.3b6, everything since 0.2)

> **Status: CLOSED — every finding is fixed, tested and on the A1200.**
> The 0.3 line grew Wasabi from a command-line tool into a daemon with
> six new commands (mouse, keys, windows, health, live view, clipboard,
> detached runs) and a web app with a server side that runs unattended
> on the NAS. This audit reads all of it. The Amiga side had one real
> memory-safety defect (a heap overflow in the live view's change list,
> reachable on tall screens), one leak (≈7 MB per viewer at daemon exit,
> measured), one out-of-bounds read on a damaged clipboard (reproduced
> and fixed on the machine), and three smaller ones. The PC/NAS side had
> no memory-safety class, but it had the server-shaped problems: bodies
> sized by the client before login, settings stored before they were
> checked (one bad value stopped every later backup night silently), and
> a file copy that could write outside the NAS's Wasabi folder.
> `audit.md`, `audit2.md` and `audit3.md` stay closed.

*Audited 2026-10-07/08, from `aae4ba1` (wasabid 0.3b5) to the fixes
committed with this file (wasabid 0.3b6). 39 commits since `8064795`.
Files: `wasabid.c` (3,922), `health.c` (256), `wasabi` (3,027),
`wasabi_view.py` (1,002), `wasabi_api.py` (719), `wasabi_monitor.py`
(295), `wasabi_backup.py` (255), `wasabi_logs.py` (178),
`wasabi_fleet.py` (158), the web app's `view/src` (3,315),
`tests/run-tests.sh` (1,073), `tests/mock-wasabid.py` (741). The C was
read by me in full; the Python service was read by a second reader in
full and every finding checked by me before it was accepted. Build:
zero warnings at `-O2 -Wall`. Offline suite: 204/204 (190 before, 14
new). Browser tests against the A1200: all green (below).*

## Stack (`-fstack-usage`, this tree)

| Function | Bytes | At 0.2 |
|---|---|---|
| `serve` | 1,552 | 1,540 |
| `main` | 572 | — |
| `cmd_speed` | 476 | — |
| `cmd_live` | 352 | new |
| `health_report` | 244 | new |
| `cmd_run_detached` | 232 | new |
| `cmd_clip` | 60 | new |

The 0.2 lesson (no large automatic buffers on an 8 KB stack) held: the
live view's picture buffers, the window list and the clipboard text are
all heap, allocated once per client and freed with it.

## Amiga side — confirmed and fixed in 0.3b6

**1. Live view: a change could overflow the send buffer.**
`live_close()` appends one rectangle per changed band to a list of
`LIVE_MAXRECT` (512). When the list was full it *widened the last
rectangle* to the full width and down to the band's end — a rectangle
whose pixels could be far bigger than the `LIVE_RECT_MAX` heap buffer
they are copied into. 512 bands means a screen taller than 1,024 lines
changing in many places at once: not the A1200's usual Workbench, but a
tall RTG screen is enough. Now a full
list records the first row that did not fit (`g_overflow_y`), and the
rest of the screen is sent as whole-width bands from there, each within
`LIVE_RECT_MAX`. Reset per frame and on fallback.

**2. Live view: picture buffers leaked when the daemon quit.** The exit
path closed every client socket but never called `live_free()`; each
viewer's previous picture and raw copy stayed allocated, ≈7 MB on the
1280×942 16-bit screen. *Measured:* free memory before start and after
quit now differ by 0 KB with a viewer connected.

**3. Clipboard: a damaged IFF chunk was read past the buffer.** The
FTXT walk advanced by the chunk's own declared size without checking it
against what was read. A program writing a chunk sized 0xFFFFFFF0 made
the next read land outside the buffer. *Reproduced* with a small test
program that writes exactly that; now the walk stops, the answer is an
empty clipboard and the daemon carries on.

**4. Live view: a failed raw-buffer allocation sent nothing.** With no
memory for the raw copy the frame size became 0 instead of falling back
to the plain (non-diff) path. Now it falls back.

**5. Live view: 16-bit screens of odd width lost their last column** to
the longword compare. Odd widths now take the byte path.

**6. Detached runs: output file names repeated after a restart.**
`T:wasabi-bg-N` restarted at 1 and overwrote an earlier run's output if
it was still being written. Now the first free name of 50 is used.

## PC and NAS side — confirmed and fixed

**7. Request bodies were sized by the client, before login.** Four
places read `Content-Length` as given. A negative value made
`rfile.read()` read until the client hung up; a huge one was allocated
up front. Both happened before the login check. Now one helper refuses
anything that is not a number, below zero (400), or over 64 KB for JSON
and 512 MB for uploads (413).

**8. Settings were stored before they were checked.** The backup's
`set_settings` assigned every field and then converted; a `keep` of
"x" raised after it was stored, and the nightly loop then raised on
every pass and died. A `time` of "3" never ran at all. The alerts had
the same shape with `temp_c`. Now both check a copy and only then take
it (bad values are a 400 and the old settings stay); the backup loop
catches everything, a failed night always clears `running` and always
raises its alert; a bad ntfy address can no longer abort a check.

**9. Copying from the Amiga to the NAS could leave the Wasabi folder.**
The destination folder was checked, but the file names inside it were
not: an Amiga path `/` came out as base name `/`, which `os.path.join`
treats as the root; `..` climbed. Now every destination is checked
(not a blank, `.` or `..` name, not an existing link, and inside the
root), and the other direction skips links on the server.

**10. One odd cookie locked the phone out.** `SimpleCookie` gives up at
the first cookie it cannot parse, and browsers share cookies across
ports, so any other app on the NAS setting one would have made the
Wasabi login vanish. Now the header is read by hand.

**11. Malformed live-view messages ended the session.** A wrong-typed
field raised inside the input worker, which stopped: the picture froze,
and a Right Amiga held down from the page stayed down on the Amiga. Now
the message is skipped. *Checked* with a raw WebSocket sending three
broken messages: the stream carried on.

**12. Pages that vanished were never noticed.** The WebSocket had no
timeout; a phone that slept left the server waiting forever, and a page
that stopped reading could stall a broadcast to every other page. Now
keepalive is on, sends give up after 30 s, an idle connection is pinged
every 30 s, and frames over 1 MB are refused.

**13. The rest, smaller:** the log hub started and stopped streams
without its lock (two pages at once could start two); `/ws/run` opened
its stop connection outside its error handling and let a command with no
time limit outlive the page (now 600 s at most); a bare Amiga name
without `:` or `/` split wrongly; `/api/settings` was readable without
logging in on the server; a night cut short left its `.partial` folder
forever; the backup's temporary name `x.part` could clash with a real
Amiga file; two edits in one second shared a backup name; secret files
were written and *then* made private (now created 0600); bad input in a
request dropped the connection instead of answering 400; a refused
HELLO left the socket open; the history's bucket size gave up to 599
points and averaged outages away (now ≤300, and any time down shows as
down); two first visitors could both set the password (now locked).

## Measured on the hardware

A1200, PiStorm32-lite/Emu68, AmigaOS 3.2, `C:wasabid` 70,148 bytes —
the size of this tree's build, installed with `wasabi update`.

- Live view (`test/live.mjs`), key settings (`settings.mjs`),
  Developer page (`developer.mjs`), clipboard (`clipboard.mjs`),
  editing/drop/mouse wheel (`files2.mjs`), two machines
  (`machines.mjs`), and the phone through `wasabi serve` (`phone.mjs`):
  all passed.
- Key press to picture on the page: median **53 ms** (48–54), the same
  as before the fixes.
- Damaged clipboard (finding 3): empty answer, daemon alive.
- Daemon quit with a viewer connected (finding 2): 0 KB lost.

## Verified correct (the coverage, so the gaps are visible)

- Daemon: every new command's payload length is checked before it is
  read; `T_KEY` text goes through `MapANSI` with dead keys; the window
  list is built under `LockIBase` and sent after unlocking (audit 3's
  lesson kept); `T_HEALTH` reads the mailbox and the Emu68 counters only
  when the device tree says Emu68; detached runs use `SYS_Asynch` and
  close their own output.
- Server: the login gates `/api`, `/ws*`, `/shots`, uploads and
  settings; `real()`/`shown()` resolve links before the root check;
  upload names are checked; the cookie is HttpOnly and SameSite=Strict;
  passwords compared in constant time; the desktop's Host, X-Wasabi and
  WebSocket Origin checks; the monitor's up/down state machine; the
  backup's hard links; the deploy watcher.

## Accepted, by choice

- **No encryption.** Wasabi speaks plain TCP with a plain key on the
  home network and the tailnet. That is the design, not a gap.
- **A screen closing during a live read or grab.** Intuition gives no
  way to hold a screen open from outside; the window is milliseconds.
  Seen zero times; now more likely because the view reads constantly.
- **Raw key codes are passed as given.** A wrong code presses a wrong
  key, as on a real keyboard.
- **`T:wasabi-bg-*` files are not cleaned up.** They are the output a
  detached run promised to keep; T: is RAM and empties on reboot.

## Weak tests

- *"nor through a link in it"* and *"the password file is private"*
  pass on the old code too: the folder-level check already caught a
  link as the target folder, and `chmod` reached 0600 in the end. They
  guard against a regression, not the original race.
- The malformed-WebSocket check (finding 11) and the ping/timeout
  (finding 12) were tried by hand, not in the suite.
- Finding 1 is correct by construction; no screen here is tall enough
  to reach it.

## The lesson worth keeping

Everything that runs unattended — the nightly backup, the monitor, a
server waiting for a phone — must treat its own stored settings and its
peers as untrusted, because nobody is watching when they go wrong. The
three worst findings on the PC side were all "a bad value got in once
and broke every later run, silently".

## Found later, not yet fixed (2026-10-10, from the AmiClaude session)

- **`discover` misses the A1200 while an emulator runs.** With FS-UAE
  running wasabid on this PC, `wasabi discover` often lists only
  `127.0.0.1 :1234 amiga`. Five tries in a row found no A1200; other
  times it found only the A1200. *Verified.*
  - Why: `discover()` probes 127.0.0.1 alongside the broadcast, and
    sweeps the subnet with unicast only when *nobody* answered. The
    A1200 often misses the broadcast, so once the emulator answers, the
    real machine is never searched for.
  - Effect: anything taking "the first Amiga found" lands on the
    emulator. AmiClaude did, and now skips 127.x itself. The CTerm
    session had to give the A1200's IP by hand.
  - Possible fix: when broadcast found only loopback answers, sweep
    anyway. Or sweep whenever the last-known real host is missing from
    the answers.
  - Also in Knowledge/amiga/toolchain-and-testing.md.
