# Plan: the discover bug, more slots, TLS (2026-10-10, proposed)

Not started. Order is the recommended one; each step is shippable alone.

## 1. Fix: `discover` misses the A1200 while FS-UAE runs

Reported in audit4.md ("Found later, not yet fixed"). `discover()` sweeps
the subnet only when nobody answered the broadcast; the emulator on
127.0.0.1 always answers, so the real machine is never searched for.

- Sweep whenever every answer came from loopback (or the last-known real
  host is missing from the answers). Keep the unicast sweep - it is the
  only thing that works on this network.
- Never pick a loopback machine as "the first Amiga found" when a real one
  is known; AmiClaude can then drop its own 127.x workaround.
- Test: a mock that answers on loopback only must still trigger the sweep.
- Client-only, no daemon change. Small.

## 2. More slots

Today wasabid takes 8 connections, but has one RUN slot ("another command
is already running"), one debug subscriber and one snoop subscriber, and
serves one frame at a time, so a big put or grab makes everyone else wait.

a. **Several RUN slots (4).** Turn the single job (g_job_active,
   g_run_client, the temp file) into an array; pump_run walks it. Each
   runner has its own process and T: file already, so this is mostly
   bookkeeping. 4 x 128 KB stack. Medium.
b. **Many listeners on debug and snoop.** Drain the ring once, send to
   every subscriber; drop only the one that stalls. Small.
c. **Long jobs stop blocking the others.** PUT/GET/GRAB are done in one
   go inside serve(). Cut them into chunks the main loop advances a piece
   at a time (per-client state). Bigger; do after a and b, and only if
   the waiting is still felt.

Not chosen: one Amiga process per connection (ReleaseSocket /
ObtainSocket). Possible, but the patches, live view and clipboard are
shared state, and threads on AmigaOS make every one of them a race.

Version: wasabid 0.4b1.

## 3. TLS (optional, off by default at first)

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
- Main-loop care: SSL_pending() must be checked as well as WaitSelect,
  or buffered data sits unread.
