# Wasabi — Full Audit

*Audited 2026-08-12, at `12b14f9` (wasabid 0.1b25). Every file read in full:
`wasabid.c` (2,838 lines), `wasabi` (1,171), `tests/mock-wasabid.py` (561),
`tests/run-tests.sh` (269), `tests/dbgemit.c`, `tests/slowwrite.c`,
`Makefile`, `README.md`, `PROTOCOL.md`. Test suite run: 50/50 passing.*

**Verdict up front:** an unusually well-crafted small project. The
documentation and design-rationale discipline are better than 95% of
open-source repos of any size, the dangerous parts (SetFunction patches,
self-update) are engineered with real care, and the test suite is genuine.
The findings include two real memory-safety-adjacent bugs in the daemon, one
race, and one place where the docs promise something the code doesn't do —
plus the usual pile of smaller observations.

## Great

- **The documentation is the standout asset.** README.md and PROTOCOL.md
  don't just describe behavior — they record *measurements* (4.5 GiB streamed
  at 20.8 MB/s, the 55 ms Wi-Fi outlier, the 17.9 MB/s bogus RAM: reading
  traced to a concurrent `info`), *failure modes discovered in practice*
  (broadcast eaten by the Wi-Fi bridge, blocking `sendto()` ARP stalls
  turning a 1 s probe into 8), and *the reasoning behind every non-obvious
  choice*. "Honest to 4 GB and wrong above it, and that is written down here
  rather than discovered later" is the house style, and it's the right one.

- **The snoop self-test** (`wasabid.c:1277`) is exceptional engineering: the
  asm trampoline's 60-byte register file and the C descriptors are two
  definitions nothing checks, so before patching anything the daemon
  `Lock()`s a nonce path and insists the captured event carries back exactly
  that string, mode, and result — refusing to run otherwise. Verified by
  deliberately mis-wiring a descriptor. That is the correct paranoia for code
  that dereferences registers in other tasks' contexts on a machine with no
  MMU.

- **The update verification ladder** (`wasabi:367`) — $VER tag, remote
  identity, nonce self-test, live handshake on a spare port — with the README
  documenting that a deliberately-broken binary passed checks 1–3 and only
  check 4 caught it. `PUT` refusing the daemon's own path via `SameLock()`
  (catches every alias) and `INSTALL` being rename-only ("the bytes that
  passed are the bytes installed") close the loop.

- **The SetFunction chain rule** (`wasabid.c:738`, `snoop_uninstall`): never
  restore a vector someone else patched over — put the interloper back, stay
  resident, warn "do NOT let this binary unload." Plus the straggler wait on
  `wasabi_snoop_users` before the code segment can vanish. This is the
  Sashimi/SnoopDOS lineage done properly.

- **Bounded I/O everywhere** (`io_wait`, `wasabid.c:469`): every blocking
  read *and write* passes through a 10 s `WaitSelect` that also watches
  Ctrl-C — with the half-frame-stall and stopped-reader failure modes
  measured on hardware. The 64 KB frame cap as "the only thing standing
  between a stray byte and a 4 GB AllocMem" is exactly right.

- **Small correctness gems**: `MODE_READWRITE` vs `MODE_NEWFILE` shared-lock
  trick for tailing run output; `pr_WindowPtr = -1` in both daemon and runner
  so requesters become wire errors instead of wedges; `vol_megabytes`
  dividing before multiplying to dodge 32-bit overflow; reading `fib_Size`
  unsigned with the rationale written down; wire-parse bounds checks in
  `get_str` that are actually complete.

- **Test suite:** 50 tests, all passing, covering handshake, round-trip
  integrity, exit codes, stream filtering, log stamping, the whole update
  ladder including the binary-that-cannot-serve case, capability negotiation
  in both skew directions, and the refusal-count UX. The mock doubling as the
  protocol reference is a smart move for a two-implementation protocol.

- **Compatibility engineering:** caps in WELCOME appended behind counted
  strings so both skew directions degrade gracefully — and both directions
  were tested against the real machine, not just argued.

## Good

- Git history is a clean narrative — one feature per commit, messages that
  state the *why* ("stop lying about files over 2 GB", "a concurrent client
  corrupts speedtest, and it already did").

- The refusal tally: counted per-address (a port scan can't grow a log
  file), rate-limited to one write/minute, persisted across restarts, and
  surfaced only when the number *rises* — "a standing count is wallpaper, one
  that moves is a signal" is good operational UX thinking.

- The client is disciplined stdlib-only Python: streaming PUT (constant
  memory for a 4 GB image), the 15-line PNG encoder over `zlib`, timeout
  removed during RUN with the reasoning documented, `resolve_target`'s
  cache→probe fallback.

- The `allow` CIDR design and the Tailscale-masquerade analysis in the
  README ("your tailnet membership is now part of wasabi's trust boundary") —
  the security section is honest about what the key does and doesn't do,
  which beats most projects' pretend-crypto.

- Error-path hygiene in the daemon: refused PUT/SPEED sinks *drain the
  in-flight body first* so DATA frames don't get reparsed as commands
  (`wasabid.c:1801`, `speed_drain`) — a desync class most protocol
  implementations discover the hard way.

## Meh

- **The built `wasabid` binary is committed** and re-committed every version
  (41 KB, ~20 revisions of it in history already). Convenient for users
  without Bebbo's toolchain; history bloat forever. A release tag/attachment
  would serve better.

- **No LICENSE file.** For a repo this public-facing in tone, that's the
  single biggest omission — nobody can legally reuse a line of it.

- **The tests mutate the real `~/.cache/wasabi/`** — `run-tests.sh:43`
  deletes the actual `last-host`, and the refusal tests overwrite
  `refused-127.0.0.1`. An `XDG_CACHE_HOME=$ROOT` export would isolate them.
  They also bind real UDP/TCP ports and lean on `sleep 1` and `timeout 1.5` —
  works locally, would flake in CI (of which there is none).

- **Mock/daemon behavioral drift** is documented for stderr (the mock
  separates, the Amiga can't) but exists undocumented elsewhere: the mock
  honors `--merge`/`--detach` flags the daemon ignores; the mock's `SCREEN`
  never errors on an unknown `--to-front` title while the daemon does; the
  mock's REBOOT/RESTART are no-ops. Fine for one developer, but "the mock is
  the reference" cuts both ways.

- `TAGNAMES` (`wasabi:35`) is dead code, and its `globals()` comprehension
  collides (`VERSION=1` vs `HELLO=0x01`) — harmless only because nothing uses
  it.

- `local_ipv4s` (`wasabi:813`) hardcodes Linux `SIOCGIFADDR`/`SIOCGIFNETMASK`
  ioctls — the discovery sweep silently finds nothing on macOS/BSD. The
  README says Linux client, so acceptable, but a one-line comment would save
  someone an hour.

- Silent truncation: RUN commands are cut at 511 bytes by `strncpy`
  (`wasabid.c:634`) with no error; the client could refuse client-side since
  it knows the wire.

- `send_err` always attaches the current `IoErr()` (`wasabid.c:561`), so
  protocol-level errors like "bad key" can carry a stale, unrelated AmigaDOS
  error number that the client dutifully prints.

- The restart relaunch (`wasabid.c:2833`) builds `Run >NIL: %s %d` with an
  unquoted `GetProgramName()` — a daemon started from a path with spaces
  won't come back after `restart`.

- 2,838 lines in one C file is a deliberate, documented choice ("one file")
  and still coherent — but it's at the outer edge; the snoop machinery alone
  is a natural split point.

- `put`'s "atomic" claim has a hair of slack: AmigaDOS `Rename` won't
  clobber, so the code does `DeleteFile(path)` then `Rename(tmp, path)`
  (`wasabid.c:1847`) — a crash between the two leaves *no* binary rather than
  a half-written one. The README's claim ("never a half-written binary") is
  technically kept, but "atomic" oversells by one syscall.

## Bad

1. **Out-of-bounds write via `drop(-1)`** — `wasabid.c:697` +
   `wasabid.c:2739`. `pump_run()` sets `g_run_client = -1` *before* sending
   the final `T_EXIT` frame. If that send fails (the client hung up just as
   the command finished — a Ctrl-C'd `wasabi run` makes this reachable),
   `pump_run` returns FALSE and the main loop calls `drop(g_run_client)` →
   `drop(-1)` → `g_clients[-1].fd = -1; g_clients[-1].hello = FALSE;` — a
   write to whatever the linker placed before the array, on a machine with no
   memory protection. It also trips `debug_stop()`/`snoop_stop()` as a side
   effect (idle stream clients are also `-1`, and `-1 == -1` matches),
   tearing down an unrelated client's stream. Fix is small: send EXIT first,
   or snapshot the client index before clearing the global.

2. **Orphaned-runner race corrupts the next RUN** — `drop()`
   (`wasabid.c:2522`) frees the run *slot* when a client disconnects
   mid-command, but the runner process is still alive and still holds a
   pointer to the global `g_job`. A new RUN then `memset`s `g_job`
   (`wasabid.c:633`) while the old runner may still write `done`/`rc`/`ioerr`
   into it and `Signal()` the daemon — so the new command can report the old
   command's exit code, or "finish" before it ran. Also: once the client is
   dropped, nothing ever deletes that run's `T:wasabi-run-N` file — a slow
   leak into T:. The single-run invariant the README advertises is only
   enforced while the client stays connected. A `g_job_active` flag that
   survives client disconnect (refusing new RUNs until the runner signals)
   would close both holes.

3. **REBOOT doesn't do what PROTOCOL.md says it does** — `wasabid.c:2247`.
   The doc: "flushes every mounted volume, waits for the socket to drain,
   then calls `ColdReboot()`"; flags bit 0 "requests a cold reboot". The
   code: `(void)flags; Delay(25); CloseSocket(g_clients[0].fd);
   ColdReboot();`. No volume flush at all (this is the one gap with
   data-integrity implications — rebooting over dirty filesystem buffers),
   the cold/warm flag is ignored so `--cold` is cosmetic, and it closes
   **slot 0's** socket rather than the requesting client's — if the requester
   landed in another slot, an innocent bystander's connection is closed
   instead. Either implement the flush (walk the DosList, `Lock`+`Info` or
   `ACTION_FLUSH` each volume) or fix the doc to say what actually happens;
   the doc as written makes a promise someone will rely on.

4. **`ls` timestamps shift by the client's timezone** — `wasabi:136`. Amiga
   `DateStamp` is the machine's local wall time; `amiga_date` converts it
   against a UTC epoch and then renders with `time.localtime()`, so on a CET
   box every real-Amiga file shows 1–2 h off. It should be `time.gmtime()`
   (render the Amiga's wall time verbatim). The mock hides this because it
   generates stamps from Unix `st_mtime`, for which `localtime` *is*
   correct — a neat example of the mock diverging from iron and blessing the
   wrong behavior.

## Smaller notes

- The discovery responder hardcodes the name `amiga` (`wasabid.c:2365`), so
  two Amigas are indistinguishable by name in `wasabi discover`; and the
  client's `found` dict is keyed by IP, so two daemons on one host (different
  ports) collapse to one entry.
- `warn_if_refusals_grew` never warns on a count that *reset* (tally file
  cleared) — deliberate-looking, fine.
- Client `_errtext` will raise a raw `struct.error` on a malformed ERR
  frame — cosmetic, the connection was garbage anyway.
- `refusals_save`'s once-per-minute gate compares `ds_Minute` for equality
  with an initial `0`, so the first save in the minute after midnight is
  suppressed — trivia-grade.
- `Close(fh)` with `fh == 0` in the speed source error path
  (`wasabid.c:2200`) — safe on OS 2.0+ where `Close(BNULL)` is a no-op, but
  worth a guard for symmetry with the rest of the file's care.

## What to do next, in order

1. Fix the `drop(-1)` path (a three-line reorder in `pump_run`).
2. Fix the orphaned-runner slot/`g_job` race + the leaked `T:` file.
3. Reconcile REBOOT with its documentation (and target the right client's
   socket).
4. `gmtime` in `amiga_date`.
5. Add a LICENSE.
6. Isolate the test suite's cache writes (`XDG_CACHE_HOME`).

The through-line: the hard, dangerous 10% of this codebase (patching,
self-update, bounded I/O) got 90% of the rigor and it shows — the bugs that
remain are all in the mundane connective tissue (client-slot bookkeeping, an
error path, a doc drifting from code). That is the right failure distribution
to have, and all four real findings are cheap to fix.

---

*Postscript: the four "Bad" findings were fixed in wasabid 0.1b26, the
commit after this audit landed — EXIT now goes out before the run slot is
cleared; the slot is gated on `g_job_active`, which only a finished runner
frees (and the headless pump deletes the orphaned `T:` file); REBOOT
flushes every volume via `ACTION_FLUSH`, closes the requester's own
socket, and the protocol doc now says plainly that every reboot is cold;
`amiga_date` renders wall time via `gmtime()`, with the mock generating
real local-wall DateStamps to match the iron. The `ColdReboot()` flush
behavior still needs a pass on the real A1200; everything else is covered
by the suite (50/50) and a manual timezone check.*

*Second postscript, at 0.1: the rest of the ledger closed over b27–b31,
and the flush-before-reboot path ran live on the machine. The cleanup
pass (b27) took the whole Meh tier — MIT license, binary out of git,
error-code honesty, hostname discovery, XDG paths, test isolation, the
update-over-VPN and WELCOME-guard finds — and the exit-with-a-live-
runner hazard flagged under "what's left" became the b30 guard and the
b31 `--force`. The client also grew macOS and BSD portability. Still
open, and only these: CI when the repo has a remote, and the
documented-by-design choices above.*
