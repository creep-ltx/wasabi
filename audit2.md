# Wasabi — Second Audit (the 0.2 line)

> **Status: CLOSED.** Nine confirmed defects, four of them
> memory-safety, all fixed in 0.2b10 and verified on the real A1200.
> Two of the auditors' claims were rejected on evidence and are
> recorded below with the reasoning, because a rejected finding is
> worth as much as a fixed one if the reasoning is written down. Two
> hazards are accepted by choice and documented. The first audit
> (`audit.md`, wasabid 0.1) stays closed and untouched; this is its
> successor for everything built on 2026-08-13.

*Audited 2026-08-13 at `dd0a54a`, fixed at `8064795` (wasabid 0.2b10).
Four adversarial reviewers in parallel, one per surface: the snoop patch
machinery, the guru report and teardown, the daemon core, and the Python
client. Files: `wasabid.c` (2,678), `patches.c` (1,534), `patches.h`
(95), `wasabi` (1,689), `tests/run-tests.sh` (459),
`tests/mock-wasabid.py` (627). Test suite: 86/86. Every finding
re-verified against the source before it was believed.*

**Verdict up front:** the 0.2 line added a great deal — a guru report
that survives a reboot, snoop grown from 14 patched calls to 30, entry
logging, three output modes — and then split ~1,300 lines of
vector-hijacking code out into `patches.c` behind a deliberately narrow
header. The audit's real subject is the day's own damage: the refactor
and the features shipped **four** memory-safety bugs, three of which
were the *same class* — a large automatic buffer on an 8 KB stack — and
one of which had already been found and fixed by hand hours earlier in
a different function. That repetition is the finding behind the
findings.

## The recurring hazard: an 8 KB stack

`wasabi ps` reports the daemon's stack as **8192 bytes**. That number
governs this codebase and nothing in the build enforces it.

Measured with `-fstack-usage` (not estimated), the worst chain before
the fixes:

```
main             488
serve           3976   every cmd_* is inlined into it at -O2
exit_refused     108
pump_run        4148   <- UBYTE buf[RUNBUF], still automatic
              ------
                8720   before send_frame/send_all/io_wait and 7 return
                       addresses, on an 8192-byte stack
```

Reachable by `wasabi quit --force` or `restart --force` while a RUN is
finishing — `exit_refused → force_run_down → pump_run` — and the first
thing `pump_run` does is `Read()` 4096 bytes into that buffer, so the
filesystem writes several hundred bytes past `tc_SPLower` into a
neighbouring allocation. The same signature as the `g_key` corruption
that cost the operator two daemon restarts and one wrong diagnosis
earlier the same day.

The multiplier was `screen_send_planar`'s `ULONG pal[256 * 3]` — 3 KB,
automatic, and because `-O2` inlines every command handler into
`serve()`, **charged to every command the daemon serves, `ping`
included**. Both are `static` now:

| | before | after |
|---|---|---|
| `serve()` frame | 3976 | **1540** |
| worst chain | 8720 | **~2400** |

**The rule, stated once so it stops being rediscovered:** in this
daemon, any buffer over ~512 bytes is `static`, never automatic. One
task, one thread, never re-entered — statics cost nothing here, and the
only thing they can't survive is re-entrancy, which was checked: every
frame sender runs in the daemon task, and the runner process never
touches the wire.

## Confirmed and fixed

**1. `pump_run`'s 4 KB automatic buffer** (`wasabid.c:672`) — above.

**2. `screen_send_planar`'s 3 KB automatic palette** (`wasabid.c:1369`)
— above. Worth noting for its own sake: *any* future large local in
*any* handler is silently added to the cost of every command.

**3. `" allow any"` writes 7 bytes past `g_extra_args[128]`**
(`wasabid.c:2287`). The allow-loop stops at `n >= 88`; a name append can
carry `n` to 124; `" allow any"` then needs 11. `g_allow` begins at the
very next byte in BSS, so the tail silently rewrote the running
daemon's own allow-list. Now bounded like the two appends above it.

**4. The restart relaunch line, up to 273 bytes into `cmd[160]`**
(`wasabid.c:2650`). 13 fixed + a 127-char program path + a 5-digit port
+ 127 chars of replayed arguments. Overflows `main`'s frame on the one
path that then either returns or parks forever. Buffer is 288 now.

**5. A screen title goes unbounded into `line[220]`**
(`wasabid.c:1459`). The title belongs to whatever program opened the
screen — several put a full path there. Bounded with `%.150s`. This was
the only unbounded OS-controlled `sprintf` left; every other one
(`fib_FileName` into 512, ps names into 160, volume names clamped) was
already correct.

**6. The snoop self-test could fail spuriously — and then lie about
why** (`patches.c`, the scan loop). During its window `g_snoop_self` is
`NULL`, so *every task on the machine* is recorded. The scan filtered
only on function identity, so a foreign `Lock()` failed the string
compare and aborted the whole test, reporting **"the patch trampoline
and this build disagree, so snoop will not run"** because Workbench
touched a drawer. It skips foreign events now. Intermittent by nature,
and likeliest on a busy machine — which is exactly when snoop is
wanted.

**7. The self-test proved only half the register file** (same area). It
called nothing but `Lock()`, exercising `RF_D1`/`RF_D2`/`d0`. Thirteen
descriptors take their string from **A0 or A1** — every `OpenLibrary`,
`OpenDevice`, `FindPort`, `FindTask`, the whole of icon.library,
`OpenDiskFont` — and none was covered, nor were `SNF_DEREF1` and
`SNF_LEN1`. A mis-typed index there would dereference a stranger's
register as a string, in their task, on a machine with no MMU, with the
self-test still green. A `FindPort()` probe on the same nonce string now
covers the A-half: exec-only, no disk, returns NULL for a name nobody
has.

**8. Libraries closed before the straggler wait** (`patches.c`,
`snoop_uninstall`). `snoop_closelibs()` dropped icon.library and
diskfont.library to `OpenCnt` 0 *before* the daemon waited for callers
still inside the stubs — inviting an expunge under a task's feet. They
are released last now, by `patches_closelibs()`, after the wait.

**9. A trial instance rewrote the live daemon's refusal table**
(`wasabid.c`, `refusals_load`/`refusals_save`). The `trial` gating added
earlier that day covered every shared *vector* and the shared black-box
*memory*, but missed the shared *file*. Gated now.

### Also fixed, from the guru/teardown reviewer

- **`guru_release()` freed the black box even when the Alert patch could
  not be removed** — handing the region back to the free list while a
  live hook still writes into it, which re-creates precisely the
  `AN_MemCorrupt` the AllocAbs ownership was introduced to prevent. It
  now refuses while `g_alert_patched`, and clears `g_stash` *before* the
  free rather than after.
- **Two early `return RETURN_FAIL` paths leaked the claimed region.**
  Start the daemon before the TCP/IP stack is up and the 256 bytes stay
  allocated for the rest of the boot, so every later daemon gets
  `AllocAbs` = NULL and the machine has **no black box for the whole
  session, silently**.
- **A pending recoverable alert masked a subsequent DEADEND**, black box
  included — losing the only report that survives a reboot. A deadend
  now displaces a pending recoverable.
- **The stash was unreadable whenever the claim failed.** Ownership is
  needed to write it, never to read it. It is read unconditionally now;
  only the "seen it, forget it" store stays gated.
- **`patches_stuck()` ignored `snoop_busy()`.** The patched set includes
  `SystemTagList` and `Execute`, which run whole commands, so a task can
  sit inside a stub for minutes; the exit path waited 2 seconds and then
  unloaded the segment out from under it. `snoop_busy()` now counts as
  stuck, and the wait is 5 seconds before parking.
- **Lost-event counters** were read-modify-written outside `Disable()`,
  racing the producer — the counter is the only evidence the ring was
  too small.

### Also fixed, client side

- **Repeat folding was never reset across a reconnect**, so the guru
  line the daemon replays on a fresh subscription — byte-identical to
  the last line before the machine died — was folded away. That defeats
  the entire point of auto-reconnect: *the terminal that watched a
  machine die should greet it coming back and say why*.
- **`NOISE_RE` was applied to the debug stream too**, and its
  `FindTask\(` arm is an unanchored substring, so `--output minimal`
  deleted a driver's own KPrintF line containing "FindTask(". It is
  snoop-only now.
- `debug --with-snoop` never checked the `snoop` capability; `cmd_update`
  raised `IndexError` on blank `Version` output instead of aborting
  cleanly; DOS errors 303/304/305 (`dos/dosasl.h` — Break and
  not-executable are routine in a trace) were missing; `_ALERT_RE`
  mangled a 9-hex-digit code.

## Rejected, with the reasoning

**"Move the 216-byte event copy out of `Disable()`."** The suggestion
was that only the head/tail update needs interrupts off. It does not:
the ring is **multi-producer** — any task on the machine can be inside
the patch — and `Disable()` is what makes claiming the slot and filling
it a single atomic act. Releasing it between the two lets two tasks
write the same slot. The interrupt latency (a few µs on PiStorm) is the
price of correctness, and the comment now says so.

**"`--ignore-wasabi` misses the runner, which is named
`wasabi-runner`."** Tested against the mock, true; against the machine,
false. The runner is created with `NP_Cli, TRUE`, so snoop names it by
its inherited CLI name `c:wasabid`, which the filter does catch —
**zero** occurrences of `wasabi-runner` in the day's real captures. The
`ln_Name` fallback is reachable in principle, so it was added to the
pattern anyway, for nothing.

But the reviewer's *deeper* point was right and worth more than the
finding: **the test passed for the wrong reason.** The mock gave its
noisy samples the task name `wasabid`, which the real daemon never
emits — it skips its own task. The mock now carries realistic names
(`c:wasabid`, `wasabi-runner`, `T:wasabi-run-`), and the other half of
the contract is asserted for the first time: that `--ignore-wasabi` must
**never** hide the guru report.

## Accepted, by choice

- **The stub's "off" path does not bump `wasabi_snoop_users`** — six
  instructions with no waits, so a task can be preempted there and be
  invisible to the unload gate. Bumping it would cost a
  read-modify-write on every call through a patch that is supposed to be
  idle. The probability is negligible against the cost; recorded rather
  than fixed.
- **`pat_match` is exponential on nested wildcards.** `#?#?#?…` against
  a 31-char task name is operator-supplied self-harm from a 64-byte
  pattern; recursion depth is bounded, only time is not.

## Verified correct (the coverage, so gaps are visible)

- **All 30 `SNOOP_DESC` entries** — every LVO and register assignment —
  re-checked against the NDK `.fd` files by parsing the bias chains, not
  from recollection. All correct, including the non-obvious ones:
  `FindToolType` taking its string from A1 (A0 is the `char **` array),
  `CurrentDir`'s D1 treated as a raw BPTR and never dereferenced,
  `MakeLink`'s `SNF_SOFT2` matching AmigaDOS semantics, `OpenDiskFont`'s
  `ta_Name` double-indirection.
- **The trampoline against the `RF_*` indices**, instruction by
  instruction, both paths — including that the saved register file still
  holds the caller's *pre-call* values when `snoop_record` reads them.
- **`snoop_format`'s worst case**: 243 bytes + NUL against
  `SNOOP_LINE_MAX` 320, computed from actual field sizes and every
  format branch. No descriptor has both a str2 and a numeric kind, so
  those maxima cannot stack.
- **Patch-context stack headroom**: ~330 bytes used against the
  800-byte check, with `snoop_record2` correctly `noinline` so the
  216-byte event does not exist when the check runs.
- **Re-entrancy**: nothing reachable from the patch path calls a patched
  function.
- **The client's filters on all 12 combinations** of `--output` ×
  `--ignore-wasabi` × single/combined, stdout and log separately.
- **`CPU_ALERTS`, `ALERT_SUBSYS`, `ALERT_CAUSE` and `DOS_ERRORS`**
  against `exec/alerts.h` and `dos/dos.h`.
- **Slot lifecycle**: every setter and clearer of the four client
  indices; a reused slot can no longer be aliased.

## Weak tests, strengthened

The suite went 84 → 86, but the point was quality, not count:

- The two `--ignore-wasabi` tests were green while the feature could
  have been a no-op in production (above).
- `NOISE_RE` had coverage for exactly one of its four arms.
- The `T:wasabi-run-` arm had none.
- **Nothing asserted the negative contract** — that the guru report, the
  lost-frame counts and the farewell survive the filters. That is the
  half of the feature that matters when a machine dies.
- `--output full folds nothing` is a pure negative grep: it passes if
  full mode emits nothing at all. Left as-is, noted here.
- The guru-decode test runs on ~0.3 s of margin against the mock's tick
  and will flake on a loaded machine — in the pass-to-fail direction, at
  least.

**A structural limit worth stating:** the suite runs the *Python client*
against a *host mock*. No test in it can see a daemon-side bug — not the
stack overflow, not the stale index, not the AllocAbs mismatch. Every C
finding in this ledger came from reading and from the real A1200. The
canary that caught the corruption in practice was `wasabi ping` failing
with "bad key", because `g_key` sits immediately after `g_clients[]`.

## Verified on the A1200 after the fixes

Update restarts in ~2 s, three times running. Snoop starts cleanly three
times in a row with the hardened self-test. Entry mode pairs 85 lines.
The icon.library and diskfont.library patches still report
(`GetDiskObject`, `OpenDiskFont("topaz.font")`). `screen` and `grab`
(1280×960, 3.7 MB raw) still work after their buffers moved. The
deadend guru path was re-proven end to end after the refactor and these
fixes — `alertemit deadend` raised a real `#8035c0de`, the machine
showed the red screen, rebooted, and the black box carried the report
across into `T:lastguru` and onto the next stream attach. That path had
last been proven on 0.2b3, before `patches.c` existed and before the
audit changed `guru_release`, `guru_boot_check` and the claim; code
rewritten twice and tested only by inference is not tested. The
corruption canary held through: heavy commands during an entry-mode
combined stream, twelve rapid subscribe/drop cycles, two concurrent
streams dropped out of order, twelve simultaneous connections against an
8-slot table, a 200-character path, and `restart --force` with a RUN in
flight — the chain that finding 1 was about.
