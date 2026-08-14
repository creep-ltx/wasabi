# Wasabi — Third Audit (0.2b10, after the guru PC capture)

> **Status: OPEN — two defects to fix, and the release blocker cleared.**
> Findings 3 and 4 reproduce on the A1200. Findings 1 and 2 are correct
> by construction but the machine says they are not reachable, and this
> audit was wrong to lead with them — see *Measured on the hardware*.
> None of it is memory-safety.
>
> **The deadend black box is proven end to end on the resized claim**, on
> this binary, with the reboot in between: the report survived in fast
> RAM, the reconnecting stream greeted the machine coming back and said
> why it died, and the crash was retired after one read. That was the one
> thing standing between 0.2b10 and a tag. The class
> that cost the 0.2 line four bugs in one day — a large automatic buffer
> on an 8 KB stack — is now clean: every `sprintf`, `strcpy`, `memcpy`
> and `strncpy` site in both C files was checked against its target
> buffer, and all 77 of them are bounded. What is left is two system
> locks held across a blocking network wait, one error frame that dresses
> itself in an unrelated AmigaDOS code, one file left on a volume when a
> transfer is interrupted, and a header contract the code does not keep.
> Two hazards are accepted and recorded. `audit.md` (0.1) and `audit2.md`
> (the 0.2 line to `8064795`) stay closed; this covers the eight commits
> since, and re-reads everything.

*Audited 2026-08-14 at `bbf8b9d` (wasabid 0.2b10). Files: `wasabid.c`
(2,679), `patches.c` (1,685), `patches.h` (95), `wasabi` (1,689),
`tests/run-tests.sh` (487), `tests/mock-wasabid.py` (632). Read in full,
one reader, no delegation. Test suite: 87/87 green. Build: clean, zero
warnings at `-O2 -Wall`. Stack figures below are `-fstack-usage` output
from this tree, not estimates.*

*Findings were then taken to the A1200 (192.168.68.109, `wasabid 0.2b10`,
`C:wasabid` 53,344 bytes — the same size as the HEAD build, so the
machine is running the guru-PC-capture code). What the hardware said is
in **Measured on the hardware** below, and it did not agree with this
audit on every point.*

## The one that would be felt — if it could be reached

**Downgraded after testing. Read this section together with the
measurements below: the mechanism is real, the reachability is not.**

`io_wait()` bounds every blocking send at **10 seconds** — that is its
whole purpose, and the comment above it explains why a daemon with one
loop cannot afford an unbounded one. But two command handlers call
`send_frame()` — and therefore `io_wait()` — while holding a lock that
belongs to the whole machine.

**1. `cmd_screenctl` sends inside `LockIBase()`** (`wasabid.c:1466`).
The screen-list walk locks Intuition's base, and the `send_frame` for
each screen's line is inside the loop. A client whose socket will not
accept bytes right now parks the daemon in `WaitSelect` for up to ten
seconds *with Intuition's global lock held* — no window opens, no menu
draws, no screen changes anywhere on the Amiga for the duration.
`LockIBase`'s own autodoc says to hold it as briefly as possible; a
network wait is the opposite of that.

**2. `ls_volumes` sends inside `LockDosList()`** (`wasabid.c:1117`).
Same shape, narrower blast radius: an `LDF_READ` lock admits other
readers, so ordinary `Lock()` calls are unaffected, but every DOS-list
*writer* blocks — a mount, a dismount, a filesystem creating a volume
node when a disk is inserted. Ten seconds per volume, and the loop is
per volume.

The fix is a reorder, and the codebase already knows it: **`info_volumes`
does exactly this correctly**, thirty lines above, and says so —

> *Names are collected under the DOS list lock and everything else is
> done after releasing it: `Lock()` itself wants the DOS list, and taking
> it twice is how a machine stops responding.*

`flush_volumes` follows the same discipline for the same reason.
`cmd_grab` follows it too: it reads `FirstScreen` under `LockIBase`,
releases, and only then ships megabytes. Three functions get it right
and two do not, in one file.

**What the machine said.** The reasoning above stops at "the socket
buffer has to be already full". It is worth asking how full, and the
A1200 answers: **not attainable at these payload sizes.** A probe that
completed the handshake, asked for a transfer and then read *nothing*
for 14 seconds was never dropped — 4 MB arrived intact afterwards, and
an earlier 53 KB `GET` went through the same treatment untouched.
`io_wait` restarts its 10-second window on every successful `send()`, so
a stalled reader has to hold the window shut for ten unbroken seconds
before anything gives, and neither probe managed it. Against that,
`ls_volumes` emits about 150 bytes for this machine's five volumes and
`cmd_screenctl` a line per screen; measured round trips are **40–43 ms**
for both, of which the lock hold is a fraction of a millisecond.

So: real by construction, unreachable in practice, and the correct
pattern is already in the file thirty lines away. Worth the two-line
reorder when either function is next touched — it costs nothing and
removes a footgun for whoever later makes one of them emit more. Not
worth calling a defect, and this audit was wrong to lead with it.

## Confirmed — and fixed in 0.2b11

**3. `cmd_kill`'s "ambiguous" error carries a stale `IoErr()`**
(`wasabid.c:981`). The daemon distinguishes two error senders on purpose:
`send_err()` for "a DOS call just failed, and `IoErr()` belongs to this
error", `send_perr()` for the daemon's own refusals — with a comment
saying why a leftover `IoErr()` must not dress a refusal in "an unrelated
AmigaDOS error number the client then dutifully prints". The ambiguous-
match path uses `send_err()`. Nothing failed; `ps_collect()` makes no DOS
call at all, so the code returned is whatever the last DOS call on this
connection left behind. After a failed `ls`, `wasabi kill Shell` on a
machine with two Shells reports:

```
ambiguous - several tasks match; use the 0x address from ps (Error 205: Object not found)
```

Every other refusal in the same function — "no task by that name", "that
is wasabid itself", "already gone" — correctly uses `send_perr`. This is
the one exception, and it is the exact failure the convention was written
to prevent. **Fixed**: `send_perr`, with the reason recorded beside it.

**4. An interrupted speedtest read-back leaves its file behind**
(`wasabid.c:1654`). In `cmd_speed`'s source half, a failed `send_frame`
closes the handle and returns — without the `DeleteFile(path)` that the
normal completion path (line 1663) and *every* error path in the sink
half (1674, 1681, 1692) perform. Ctrl-C during `wasabi speedtest 256MB
--target Dump:` therefore leaves a 256 MB `wasabi-speed.tmp` on the
volume. It does not accumulate — the next run opens `MODE_NEWFILE` and
truncates — but until then a quarter of a gigabyte is gone from a volume
the operator was measuring precisely because they cared about its space.
**Fixed**: the failed-send path closes *and* deletes, like every other
error path in the same command.

*Both fixes carry a version bump to **0.2b11**. `0.2b10` already named
two different binaries — the audit build at `8064795` and the
PC-capture build at `d192b8f` — which is a defect in its own right:
`discover`, `info` and `update` all report the version, and none of them
could tell those two apart. Shipping a third under the same name would
have compounded it.*

**5. The guru PC capture could never have found a frame** (`patches.c`,
`wasabi_alert_note`). Found after this audit closed, by taking the
decoder to an emulator and giving it a frame the CPU actually pushed —
`tests/pcprobe.c`, FS-UAE with a 68030 and the JIT off.

The hook copies the supervisor stack starting at `&probe`, a one-byte
local. `guru_find_frame` scans that copy **at even offsets only**
(`i += 2` from 0), which is correct: Exec pushes its exception number at
an even address, because the 68k keeps the stack even. But nothing made
the *origin* even. The compiler placed `probe` at an odd address — on
both platforms, measured: `0x40002313` under FS-UAE and `0x080022F3` on
the A1200 — so every field of the frame landed at an odd offset inside
the snapshot and the scan stepped straight past it.

Rounding the origin down to even recovered the PC exactly, checked
against ground truth on both machines:

```
pcprobe: 38 byte(s) captured from 0x080022F3 (ODD - realigned before copy)
pcprobe: decoded PC 0x0925FB0E, vector 33, format $0
pcprobe: expected PC 0x0925FB0E (the instruction after the TRAP)
```

**The reason this survived is worth more than the bug.** The failure is
silent and indistinguishable from the correct answer on the primary
target: "no frame found" is exactly what an Emu68 alert legitimately
produces, since CPU faults never reach the guest there. `dectest.c`
could not catch it either — it builds frames at even offsets by
construction, which is precisely the assumption that was untested. A
feature that has never once produced output has never been tested, no
matter how green its unit tests are.

**Fixed** in 0.2b13, one line, with the measurement recorded beside it.
`dectest.c` gains a case asserting a frame at an odd offset is *not*
found, which documents the invariant the capture side must keep.

**6. `snoop_next_line` discards the ring instead of refusing it**
(`patches.c:1009`, against `patches.h:41`). The header states the
contract:

> *`snoop_next_line` needs a buffer at least this big; a smaller one is
> refused rather than truncated, because a half-formatted trace line is
> worse than no line.*

The code neither refuses nor truncates: when `max < SNOOP_LINE_MAX` it
falls through the format call, **advances the tail anyway**, and loops —
draining and silently destroying every event in the ring before returning
0. The daemon always passes `SNOOP_LINE_MAX`, so this is latent today.
It is listed because the header is what the next caller will read, and
what it promises is not what happens.

## Measured on the hardware

*A1200 + PiStorm32-lite/CM4, Emu68, `wasabid 0.2b10`, 2026-08-14. The
daemon reports an **8192-byte stack** and **priority 1**, as designed.*

- **Finding 3 reproduces exactly, including the mechanism.** Two
  `C:Wait` CLIs started, one failing `ls DH0:no-such-drawer-xyzzy` to
  leave `IoErr()` at 205, then an ambiguous kill **on a separate
  connection**:

  ```
  wasabi: cannot lock that path (Error 205: Object not found)
  wasabi: ambiguous - several tasks match; use the 0x address from ps (Error 205: Object not found)
  ```

  The stale code crossed both a command boundary and a TCP connection,
  because it lives in the daemon's process and nothing in `cmd_kill`
  touches DOS.

- **Finding 4 reproduces, at full size.** `speedtest 64MB --target
  Dump:`, client killed 0.8 s into the read-back, then:

  ```
  - 67108864 ----rwed  2026-08-14 21:58  wasabi-speed.tmp
  ```

  Exactly 64 MB stranded. A clean run of the same command leaves
  nothing, confirming the completion path deletes and only the failed-
  send path does not. (Removed afterwards.)

- **Findings 1 and 2 do not reproduce, and cannot here.** See above.

- **The deepest stack chain runs correctly at full length.** This audit
  measured `main → serve → stream_greet → guru_read_note` at 2872 bytes
  but could not exercise it, because it only executes when `T:lastguru`
  exists — and `T:` was empty. Writing a **319-byte** note (exactly what
  `guru_read_note` will `Read`) and subscribing produced a greeting of
  **340 bytes** — `20 + 319 + 1`, the arithmetic exactly — untruncated to
  the final byte, with the daemon answering a ping normally afterwards.
  The chain fits the 8 KB stack in practice as well as on paper.

- **The hardened snoop self-test starts clean 5 times out of 5**, which
  is the `audit2` finding-6 fix holding on a live machine.

- **A real capture, made while `List SYS:C` ran**, fired 10 of the 30
  descriptors — `OpenLibrary` (148), `FindTask` (77), `GetVar` (20),
  `SetVar` (10), `CurrentDir` (8), `Lock` (6), `Open` (5),
  `SystemTagList`, `RunCommand`, `LoadSeg` (2 each). No malformed lines,
  no lost-event notices.

- **`audit2`'s `--ignore-wasabi` rejection re-confirmed independently.**
  In this build's real captures the runner names itself **`c:wasabid`**
  — 10 occurrences, zero of `wasabi-runner`, exactly as that ledger
  found — and `--ignore-wasabi` removes all of them (0 remaining).

### The recoverable guru path, end to end

Run with the operator at the machine, 2026-08-14 22:09. `tests/alertemit`
uploaded to `T:alertemit`, a `wasabi debug` stream attached, then
`Alert(0x35000000)` raised from a real process:

```
[wasabi: RECOVERABLE ALERT #35000000 (unknown subsystem) in task 'T:alertemit' (0x092600f0)]
```

- **The report won the race.** It was on the wire ~2 s after the command
  was issued and *before* the alert display took the machine — which is
  the whole reason the daemon runs at priority 1.
- **No PC, and that is the correct answer.** This is the first hardware
  test of the `d192b8f` guard: `alertemit` calls `Alert()` from user
  mode, so the supervisor-stack bounds check fails and the hook records
  nothing rather than decoding the task's own stack into a plausible,
  invented PC. The line ends at the task address, exactly as that commit
  claimed and nobody had yet checked.
- **The culprit is named by its CLI command** (`T:alertemit`), not a
  generic task name — the `snoop`-style naming working in the alert path.
- **`T:lastguru` was written**, stamped:
  `RECOVERABLE ALERT #35000000 in task 'T:alertemit' (0x092600f0) - 14-Aug-26 22:09:21`
- **The note replays on a fresh subscription, on both streams
  independently** — verified by attaching `debug` and `snoop` separately
  afterwards and getting the greeting on each.
- **The machine recovered fully**: alert dismissed, runner exited, run
  slot freed, `run`/`ping` normal.

One thing exercised by accident and worth recording: the test client was
killed by its own timeout *while the runner sat inside `Alert()` waiting
for the click*. The daemon did the right thing — kept the runner headless
after the client vanished, cleaned up its temp file, and freed the slot
when it finished. That is the `drop()` / headless `pump_run` contract
from `audit.md`, exercised for real rather than by inference.

Only the alert-code decode reads oddly: `guru_decode` renders
`#35000000` as *"unknown subsystem"*, which is literally what subsystem
`0x35` is called in the client's own table. Correct, and unhelpful, for a
code chosen precisely because it is not a real subsystem.

### The deadend black box, end to end, on the resized claim

Run with the operator at the machine, 2026-08-14 22:14 — the test
`audit2`'s own rule demanded, since `d192b8f` grew the claim from 256 to
512 bytes and rewrote `guru_claim`, `guru_release` and `guru_boot_check`
around it.

**The control first.** After a clean reboot following only the
recoverable alert, `T:` was empty and a fresh stream greeted with
nothing — so a recoverable alert correctly leaves the black box alone,
and no stale stash was in play. Whatever turned up next could only be
new.

Then `T:alertemit deadend` raised `#8035C0DE`, and the whole loop ran:

```
[debug: nothing heard for 20 s - the machine is frozen, rebooted, or busy with something long]
[debug: connection lost (connection closed by the Amiga) - reconnecting until the Amiga returns; Ctrl-C to stop]
[debug: reconnected after 5 s - wasabid 0.2b10]
[wasabi: last guru: DEADEND ALERT #8035c0de in task 'T:alertemit' (0x091fb680) - from before the last reboot (seen 14-Aug-26 22:14:09)]
```

- **The report survived the reboot in fast RAM.** `T:` is RAM-backed and
  the reboot wiped it: `T:alertemit` was gone afterwards and `T:lastguru`
  was the only file there, 115 bytes, stamped at the moment of the crash.
  It cannot have come from a file, because no file survived. The black
  box did its job at 512 bytes.
- **The terminal that watched the machine die greeted it coming back and
  said why** — silence detector at 20 s, reconnect at 5 s, guru replayed
  as the first line of the new subscription. That is the loop the whole
  feature exists for, observed rather than inferred.
- **No live line escaped for the deadend**, in measured contrast to the
  recoverable alert twenty minutes earlier, which reached the wire in
  ~2 s. Same machine, same daemon, same task context — the difference is
  that a deadend does not give the scheduler another turn. This is the
  clearest statement yet of *why* the black box is not redundant with the
  live report: it is not a fallback for a missing subscriber, it is the
  only mechanism that works for the alerts that matter most.
- **No PC**, consistent with the recoverable run and with the documented
  scope: `alertemit` calls `Alert()` deliberately from user mode, so the
  supervisor-stack test correctly declines to invent one.
- **The crash is retired after one read.** `T:lastguru`'s "seen" stamp
  was `22:14:09` before a `wasabi restart` and `22:14:09` after it — so
  `guru_boot_check`'s `st->magic1 = 0` held, and the next daemon did not
  rediscover an old crash. (The restart also re-proved the reload path in
  passing.)

**Nothing on the machine is now untested.** The two paths this audit
listed as unproven — the deepest stack chain and the deadend black box —
have both been exercised on the A1200 against this binary.

## Verified correct (the coverage, so the gaps are visible)

- **Every unbounded-copy site in both C files** — 77 of them: 61
  `sprintf`s, 3 `strcpy`s, 11 `memcpy`s, 2 `strncpy`s — checked against
  the size of the buffer written to, with the longest possible expansion
  of each conversion. All bounded. Worth naming the tight ones, since
  they are the ones a future edit will break: the stuck-patch warning is
  180 bytes into `w[200]` (`wasabid.c:2629`); the restart
  relaunch line is 273 into `cmd[288]` (`2666`, as `audit2` computed);
  `snoop_format`'s worst case is 245 into `SNOOP_LINE_MAX` 320, reached
  by a 31-char task name, `GetDiskObjectNew`, a 99-byte `s1`, a 63-byte
  `s2` and `") = fail (err -2147483648)"`.
- **One latent-by-luck case, recorded rather than fixed**:
  `sprintf(msg, "cannot speedtest to %.40s: %.140s", ...)` into
  `msg[200]` (`wasabid.c:1626`). The format's own precisions permit
  20+40+2+140+NUL = 203 bytes. It is safe only because `why` is
  `char[100]` at every call site, so the real maximum is 162. The bound
  that saves it is in a different function from the one that needs it.
- **The guru black box after the resize.** `d192b8f` grew the stash from
  256 to 512 bytes and the note from 192 to 320. The arithmetic holds:
  `struct GuruStash` is 260 bytes inside a 512-byte claim; the claim
  address is `(mh_Upper - 512) & ~7`, so `top + 512 <= mh_Upper` and the
  region never runs past the heap; `guru_stash_sum` covers offsets 0–255,
  i.e. every field except `sum`, which is last as its comment requires;
  and pre-aligning `top` to 8 is what makes `AllocAbs`'s licence to
  return a *lower* address a no-op, keeping the write address and the
  next boot's read address identical. The worst `g_guru_note` line is 199
  bytes of 320, and the worst `guru_take_live` line 145 of 200.
- **`guru_find_frame`'s bounds**, byte by byte. The scan guard
  `i + 12 <= len` exactly covers the three reads it makes (`snap+i..i+3`,
  `f+2..f+5`, `f+6..f+7`), and the fault-address guard `i + 16 <= len`
  exactly covers `f+8..f+11`. Offsets check out against M68000PM
  Appendix B: SR at +$00 and PC at +$02 on every 68k, effective address
  at +$08 for formats $2, $4 and $7. Formats $A/$B are undecoded and
  documented as such.
- **The deadend-displaces-recoverable logic** (`patches.c:1311`): a new
  deadend replaces a pending recoverable, a new recoverable never
  replaces anything, a second deadend keeps the first. All three arms
  read correctly out of the single condition.
- **Teardown ordering** in `main`'s `out:` block: patches removed before
  `guru_release`, `guru_release` refusing while `g_alert_patched`,
  the straggler wait before `patches_closelibs`, `patches_stuck` counting
  `snoop_busy`, and the stream indices restored for exactly one message
  and cleared again. Every early `return RETURN_FAIL` and the `goto out`
  path all reach `guru_release`.
- **Stack, re-measured.** `serve()` is still 1540 bytes, unchanged since
  `audit2` fixed it. The deepest chain is no longer the force-quit one:
  it is now `main(572) → serve(1540) → stream_greet(412) →
  guru_read_note(348)` = **2872 bytes**, the guru-note path added by the
  0.2 line. Comfortably inside 8192, and every buffer on it is under the
  512-byte rule, but it is 470 bytes deeper than the ~2400 `audit2`
  recorded and the number in that ledger is now stale. Patch context is
  unchanged at 280 bytes (`snoop_record` 24 + `snoop_record2` 256)
  against the 800-byte headroom check.
- **The client's error and alert decoding**: `protstr`'s bit assignments
  against `dos/dos.h` (active-high `hspa`, active-low `rwed`);
  `guru_decode`'s CPU branch against `exec/alerts.h`; `_ALERT_RE`'s
  `(?! \()` making `dress_alerts` idempotent against the current guru
  line format, where the code is followed by `" in task"`.
- **Frame reassembly across a timeout** (`Conn._fill`): a `socket.timeout`
  mid-frame leaves `self.buf` intact, and the retry resumes rather than
  re-reading a header it already consumed. The silence-counter path in
  `follow()` depends on this and it holds.

## Accepted, by choice

- **`snoop_record2` dereferences caller-supplied pointers** — including
  one extra indirection under `SNF_DEREF1`, and an odd address there
  would be an address error on a 68030. This is the feature: a call trace
  that will not read the caller's string argument has nothing to show.
  It is bounded by `copystr`/`snoop_copycount` and gated behind a
  self-test that proves the register file first.
- **`cmd_screenctl` uses a screen pointer after `UnlockIBase`**, exactly
  as `cmd_grab` does. `cmd_grab` says so out loud in a comment —
  *"Nothing keeps the screen from closing afterwards, so there is a race
  here… Said out loud rather than papered over"* — and `cmd_screenctl`
  says nothing. Same hazard, same acceptance; only the comment is
  missing.

## Weak tests

The suite is 87 green and the C findings above are, as ever, invisible to
it — it runs the Python client against a host mock. Two things about the
suite itself are worth writing down:

- **A skipped test reports as a pass.** `run-tests.sh:299` — when the
  host has no `gcc`, the guru frame decoder test prints
  `ok  guru frame decoder (skipped: no host gcc)` and increments the pass
  count. The suite then says "87 passed" for 86 tests that ran. The
  failure mode is a CI image losing its compiler and nobody noticing that
  the only test of the decoder stopped running. (The extraction itself is
  sound: if the `awk` ranges ever drift from `patches.c`, the compile
  fails and the test correctly goes red.)
- **`dectest.c` builds the decoder with 64-bit `ULONG`.** It typedefs
  `ULONG` to `unsigned long`, which is 8 bytes on the x86-64 hosts CI
  runs on and 4 bytes on the m68k target. Nothing in `guru_find_frame`
  depends on the width — every value is under 32 bits and no wrap is
  relied on — so the test is valid today. It is one `#include <stdint.h>`
  and `uint32_t` away from staying valid if the decoder ever does
  arithmetic that could overflow.
- `--output full folds nothing` is still the pure negative grep
  `audit2` flagged, and still passes if full mode emits nothing at all.

## What this audit did not do

Nothing is now outstanding on the hardware. What remains unexercised is
what `audit2` already recorded as unexercisable here: the guru **PC
capture** has still never produced a PC, because every alert this machine
can raise comes from user mode and Emu68 does not deliver CPU exceptions
to the guest at all. Both guru runs above confirmed the *guard* — it
declines to invent a PC — but the decode path itself remains proven only
by `tests/dectest.c` against hand-built frames. That is a platform limit,
not a gap in the work, and the README already says so.

## The lesson worth keeping

This audit led with the finding the hardware then refused to reproduce,
and it led with it for a defensible reason — the pattern is genuinely
wrong, and the same file demonstrates the right one three times. But
"wrong pattern" and "reachable defect" are different claims, and reading
alone cannot tell them apart. The two findings that survived contact
with the machine were the two that named a specific observable output;
the two that did not were the two that ended in an estimate of how full
a buffer might get. **A finding that cannot name what you would see is a
hypothesis, and should be ranked as one.**
