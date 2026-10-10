# Plan: the discover bug, more slots, TLS (2026-10-10, proposed)

Not started. Order is the recommended one; each step is shippable alone.

## 1. Fix: `discover` misses the A1200 while FS-UAE runs - DONE 2026-10-10

Reported in audit4.md ("Found later, not yet fixed"). `discover()` sweeps
the subnet only when nobody answered the broadcast; the emulator on
127.0.0.1 always answers, so the real machine is never searched for.

- Sweep whenever every answer came from loopback (or the last-known real
  host is missing from the answers). Keep the unicast sweep - it is the
  only thing that works on this network.
- Never pick a loopback machine as "the first Amiga found" when a real one
  is known; AmiClaude can then drop its own 127.x workaround.
- Test: a mock that answers on loopback only must still trigger the sweep.
- **Also (CFile session, 2026-10-10): the A1200 changed address after a
  restart (.109 -> .107)** and `wasabi ping` said "no wasabid answered"
  until a manual `wasabi discover`. When the cached address fails, the
  fresh probe must do the full sweep and retry once or twice (the Amiga
  may still be starting its network), then cache the new address.
- **Also: `wasabi grab --json` is refused.** --json is a global option, so
  only `wasabi --json grab` works, though the skill writes it after the
  command. Accept it in both places, for every command.
- Keep `wasabi discover`'s output as it is (lines starting with the IP):
  AmiClaude reads it.
- Client-only, no daemon change. Small.

## 2. More slots - a, plus freeing stuck slots, DONE 2026-10-10 (wasabid 0.4b2, on the A1200)

Today wasabid takes 8 connections, but has one RUN slot ("another command
is already running"), one debug subscriber and one snoop subscriber, and
serves one frame at a time, so a big put or grab makes everyone else wait.

a. **Several RUN slots (4).** Turn the single job (g_job_active,
   g_run_client, the temp file) into an array; pump_run walks it. Each
   runner has its own process and T: file already, so this is mostly
   bookkeeping. 4 x 128 KB stack. Medium.
   **Freeing a stuck slot without a reboot** (CFile session: a hung test
   program held the only slot until a reboot): `wasabi slots` lists what
   each slot runs and for how long; `wasabi free N` sends the program
   Ctrl-C, and if it does not end, the daemon lets go of the slot anyway
   (the hung program stays in memory, but the slot is usable again).
   Same for the CTerm session's case: `wasabi kill --force` removed the
   command's process, but the runner was left waiting for it forever and
   the slot stayed taken (even `restart --force` could not free it). A
   forced kill of a run's process must release its slot too.
b. **Many listeners on debug and snoop.** Drain the ring once, send to
   every subscriber; drop only the one that stalls. Small.
c. **Long jobs stop blocking the others.** PUT/GET/GRAB are done in one
   go inside serve(). Cut them into chunks the main loop advances a piece
   at a time (per-client state). Bigger; do after a and b, and only if
   the waiting is still felt.

Not chosen: one Amiga process per connection (ReleaseSocket /
ObtainSocket). Possible, but the patches, live view and clipboard are
shared state, and threads on AmigaOS make every one of them a race.

d. **Small ones from the CTerm session (2026-10-10):**
   - Two FS-UAE emulators both on 127.0.0.1:1234. Already possible today:
     `wasabid 1235` (a bare number is the port) and `WASABI_PORT=1235`
     or `--port 1235` on the client. To do: say so in the README, and
     make discover probe loopback on a few ports and list each emulator.
   - DONE 2026-10-10 (client): discover lists each emulator on ports
     1234-1237, README says how; `wasabi mouse wheel up|down [n]`.

Version: wasabid 0.4b1.

## 3. TLS - REQUIRED (the user's decision, 2026-10-10)

No plain fallback: the user chose TLS as a requirement over "try TLS,
else plain". So:
- wasabid without AmiSSL refuses to start and says why (no silent plain).
- Every Amiga running wasabid needs AmiSSL 5: the A1200 has it; the
  FS-UAE setups do not (none found on their drives) - install it there.
- One change-over for everything at once: A1200 daemon, the `wasabi`
  command (all sessions share it), the desktop app, the NAS/phone app.
  Message the sessions first; pair the certificates once.
- Discovery stays plain (it only says "I am here"; no key in it).
- The sections below were written for an optional TLS; where they say
  "optional", "plain stays" or "require tls setting", read "required".

### Earlier notes (optional TLS)

- **TLS 1.3 with a pre-shared key = the existing Wasabi key.** No
  certificates to make, renew or copy; both ends already hold the key.
  The key then never crosses the wire, even once.
- Amiga: AmiSSL 5 (already installed and verified on the A1200 for CTerm's
  ssh). Open it only when TLS is switched on, so a machine without AmiSSL
  still runs wasabid plainly.
- PC/NAS: Python's ssl has PSK callbacks from 3.13 (PC 3.14, NAS image
  python:3.13-alpine) - no new dependency.
- Same port; the client says "TLS" in its first bytes, so plain and TLS
  clients both work during the change-over. A `require tls` setting closes
  plain afterwards.
- UDP discovery stays plain: it only says "I am here".
- Measure first: live view frames per second and put speed, plain vs TLS,
  on the A1200. If the view slows noticeably, allow plain for loopback
  and the view only.
- **AmiClaude already runs TLS on the A1200 (measured by its session,
  2026-10-10):** handshake about 50 ms; 1 MB to the Amiga in 0.16 s, back
  in 0.12-0.13 s (6-8 MB/s); +3 KB binary. So speed is not the worry it
  was. Its code: AmiClaude/amiga/tls.c (~200 lines, AmiSSL 5) and
  tls_context()/tls_pipe() in helper/amiclaude.py. It uses pinned
  self-signed EC P-256 certificates (each end trusts only the other's;
  PARTIAL_CHAIN + NO_CHECK_TIME because the Amiga's clock is not
  trusted), TLS 1.3 only, Amiga as server.
- **Changed recommendation:** reuse that proven approach (pinned
  certificates) rather than a new PSK one; PSK stays the fallback if
  handing out certificate files to the NAS/phone turns out awkward.
- Traps it found: drive the handshake non-blocking from the select loop
  (SSL_do_handshake on each readable); one thread owns each SSL object;
  in Python wrap_socket takes over the socket's fd.
- Main-loop care: SSL_pending() must be checked as well as WaitSelect,
  or buffered data sits unread.

## Working on it: the A1200 is shared

Several sessions test on the A1200 through Wasabi at the same time. Before
putting a new wasabid on it, restarting it or rebooting: message the other
sessions, say what and when, and wait for a pause. Every step must keep
today's `wasabi` clients working (plain stays on until all have moved).
