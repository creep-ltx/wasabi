# Change log

What changed in each version, newest first. Betas (`0.10b4`) are on the
`main` branch before they are released; released versions have their
files on the [releases page](https://github.com/creep-ltx/wasabi/releases).
`wasabi ping` shows which daemon an Amiga runs.

## 0.10 (betas, not released yet)

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
