# Change log

What changed in each version, newest first. Betas (`0.10b4`) are on the
`main` branch before they are released; released versions have their
files on the [releases page](https://github.com/creep-ltx/wasabi/releases).
`wasabi ping` shows which daemon an Amiga runs.

## Unreleased

- **Wasabi phone on the NAS no longer restarts every minute while the
  Amiga is off.** `wasabi serve` stopped when no Amiga answered at
  start, so Docker restarted it once a minute (and the NAS sent an
  e-mail each time). It now serves the last Amiga it knew and connects
  when the Amiga is back. Tested on the NAS with the A1200 off.

## 0.11b2 - 17 October 2026

- **Less memory again:** about 200 KB instead of about 307 KB (measured
  on the A600 with Workbench 2.05 in FS-UAE, free memory with and
  without wasabid, 0.11b1 and 0.11b2 the same way). The 64 KB buffer
  for incoming messages is now only taken for a big one (an upload, a
  long ARexx program) or a download, and given back when it is done;
  small messages use 2 KB. Encrypted sending uses a 16 KB buffer
  instead of 64 KB (one TLS record is 16 KB at most, so nothing is
  sent differently). The live view's 64 KB buffer goes back when the
  last viewer leaves, the ARexx port's 8 KB and the screen palette's
  3 KB when their command is done, and TLS reads its key files into
  memory it gives back. wasabid is built for size, 17 KB less code;
  the screen grab, live view and SUM loops are still built for speed
  (the same machine code as before). Tested on the A1200 over TLS
  (3 MB and 16 KB-edge put/get, grab, live view at the same 23 ms a
  frame, ps, ARexx) and in FS-UAE (put/get, grab, live, ARexx, memory
  back to the same figure after each).

## 0.11b1 - 17 October 2026

- **Less memory:** wasabid took about 520 KB, half of a stock A600; now
  about 330 KB there (measured on an A600 with Kickstart 3.1 and 1 MB
  in FS-UAE: 240 KB free with 0.10, 474 KB with 0.11b1). The C library
  gave its text input and output 64 KB buffers each, which wasabid never
  uses: now 1 KB. The 64 KB buffer for encrypted connections is taken on
  the first encrypted connection, the 32 KB debug ring when a debug
  stream first starts, and the 15 KB process list only while `ps`,
  `kill` or a forced stop needs it. Nothing else changed.

## 0.10 - 15 October 2026

The four betas below, released together: timed input, gadgets by id,
ARexx programs, the Python API, MUI 3.8 windows, and OS 3.0/3.1 fixes.
Tested on the A1200 (PiStorm32-lite/Emu68, OS 3.2.3) and in FS-UAE
(OS 3.2.1 + MUI 5, 3.1 + MUI 3.8, Workbench 2.05 on an A600).

### 0.10b4 - 15 October 2026

Mostly from spotUP's list (UP-Term's test rig).

- **Timed input:** `wasabi input` sends a drag, a key chord or a burst
  of typing in one request, and the Amiga plays it with fixed gaps of
  1/50 s, so the network no longer changes the timing. Steps: `key`,
  `type`, `move`, `click`, `double`, `down`, `up`, `wait`. New
  protocol command `INPUTS` (capability `inputs`).
- **Gadgets by id:** `ui list` shows each gadget's GadgetID, and
  `ui click --id N` / `ui set --id N TEXT` use it - it stays right when
  the window is laid out differently. MUI objects report their
  `MUIA_ObjectID` there.
- **ARexx programs:** `wasabi rexx 'say 1+1'` (or `--file`) runs an
  ARexx program with `RX`: its output comes back, the exit code is its
  rc. `wasabi arexx REXX ...` and `arexx AREXX ...` now do the same
  (ARexx's own ports take a script's name, not its text, and `AREXX`
  never answers).
- **Python API:** `wasabi_client.py`, a documented, versioned API (1.0)
  for your own scripts: connect, run, files, keys, mouse, timed input,
  grab, windows, gadgets and menus, ARexx, the clipboard. See
  [docs/api.md](docs/api.md).
- **Plain again:** when an Amiga that was once reached encrypted has no
  encryption any more, the error now names the fix (`wasabi keys forget
  HOST`, or `--plain`); docs/security.md has a section on it.
- **MCP: 28 tools** - new `amiga_input` and `amiga_rexx`; `amiga_ui_click`
  and `amiga_ui_set` take an id.
- Fixed: menu items that cannot be checked no longer show as checked.
- PROTOCOL.md: the pointer position on the window list's `S` lines,
  and every capability with the version it came in.

### 0.10b3 - 15 October 2026

- **MUI 3.8 windows** are read too (buttons, checkmarks with their
  state, list rows, clicking by name).
- MUI pages that are not shown no longer use up the object budget.

### 0.10b2 - 15 October 2026

- wasabid runs on a 16 KB stack of its own when the Shell gave it less.
  OS 3.1 gives programs 4 KB, which crashed `ui list` there.
- GadTools list rows on OS 3.0/3.1 again.

### 0.10b1 - 15 October 2026

- `ui list` no longer crashes wasabid on OS 3.0/3.1 GadTools windows.

## 0.9 - 14 October 2026 (files updated 15 October)

- **Ready-made clients** for Linux (x86-64, ARM64), macOS (Apple
  Silicon, Intel) and Windows: one file each, no Python needed.
- **List rows** of GadTools and MUI lists, by text, clickable when in
  view.
- **Uploads and downloads no longer make other requests wait.**
- `wasabid.020`, a build for the 68020 and up.
- Keys: F11, F12, Home, End, Page Up/Down, Insert, the keypad, any raw
  code (`0x45`). `grab --region`, `mouse where`, `--native` coordinates.
- `wasabi info` lists the patches wasabid has in place. AmiSSL is loaded
  only when encryption keys exist.
- **Updated on 15 October, same number:** runs on AmigaOS 2.04 and
  later (tested on Workbench 2.05); `wasabi sum`, a checksum of the
  screen, a window or a region worked out on the Amiga (`wait change`
  and `wait still` use it); about 250 KB less memory; MCP 26 tools.

## 0.8 - 14 October 2026

Everything since 0.2 (the betas 0.3 to 0.7 were never released):

- **The screen:** `mouse`, `key`, `windows`, faster screenshots, and
  `wasabi view`, the live screen in a window.
- **Gadgets and menus by name** (`wasabi ui`) in Intuition, GadTools,
  ReAction and MUI 4/5 windows.
- **Apps:** `wasabi desktop` and `wasabi serve` (the same on a phone,
  with history, alerts and nightly backups).
- Four commands at once (`slots`, `free`), `run --detach`, the
  clipboard both ways, `health`.
- **ARexx both ways:** `wasabi arexx`, and the `WASABI` ARexx port.
- **MCP server** for AI assistants.
- **Optional TLS 1.3** with your own certificates.

## 0.2 - 14 August 2026

- **Crash reports:** the guru code and the dying task, kept across a
  reboot.
- The DOS call trace grew from 14 to 30 calls; entry logging; output
  modes.
- Streams reconnect by themselves; stack headroom in `ps`;
  `grab --diff`.

## 0.1 - 12 August 2026

The first release: discovery, files, run with live output and real
exit codes, deploy, debug output, the DOS call trace, screenshots
(`grab`, `screen`), tasks (`ps`, `kill`), `speedtest`, reboot, restart
and self-update.
