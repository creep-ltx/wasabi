# The desktop app and Wasabi phone

[Back to the README](../README.md)

## The desktop app

`wasabi desktop` opens Wasabi as an app: a sidebar and four pages.

- **Overview** - live: the Pi's temperature and Emu68's 68k CPU meter
  with three-minute sparklines (hover for a reading), the power and heat
  status since the Pi started, the Pi's uptime, chip and fast memory and
  every disk as meters (network volumes, which report no real numbers,
  say so), and the versions: wasabid, Kickstart and Workbench, exec,
  Emu68, the board, clocks and voltage, the JIT cache, the last guru.
  It reads `health` every 2 s and the volumes every 30 s.
- **Screen** - the live view below, with a Screenshot button. It runs
  only while the page is open: an open live view costs the Amiga ~5%.
- **Files** - this PC on the left, the Amiga on the right. Click a
  folder to open it, type a path, tick files *or folders* and copy them
  across either way. New folder on both sides; Delete on the Amiga side
  (folders with everything in them), always after asking. The safety
  catch holds here too: writing or deleting in a system place asks a
  second time. **Drop files** on either pane to put them in that folder
  (or **Upload** - a phone has no dragging). **Edit** a ticked Amiga
  text file (up to 256 KB) in place: saving first keeps a copy of the
  file as it was - on this machine, in `~/.local/share/wasabi/backups`
  (the NAS's `Backups` folder for Wasabi phone), never as .bak files on
  the Amiga; the last 20 versions per file, any of them one click back
  - and writes it with its protection bits kept (a plain upload would
  drop the script bit of a file in `S:`).
- **More than one Amiga** - the picker at the top of the sidebar (and
  on a phone's Overview) switches the whole app to another machine;
  *Manage machines* adds one by address (FS-UAE on this PC is
  `127.0.0.1`), with its own key if it has one, or by **Look for Amigas
  on the network**. The first machine is "found automatically" - wherever
  Wasabi finds it, so a new DHCP address does not lose it. The list is
  `machines.json` in the config folder (private). History and alerts on
  the NAS watch its first machine.
- **Screenshots** - full-colour, exact grabs, kept in `~/Pictures/Wasabi`;
  a gallery, a big view, download and delete.
- **Clipboard** (on the Screen page) - shows the Amiga's clipboard (what
  its Copy put there) with "Copy to this device", and sends text to it,
  optionally pasting it into the active window with Right Amiga+V. The
  browser lets a page touch this device's clipboard only on a secure
  page (this PC's app is one; the phone over plain http is not), so the
  text boxes stand in there.
- **Developer** - four tabs:
  - **Logs**: the debug output (serial / KPrintF) and the DOS calls
    (snoop) as they happen, side by side, timestamped; tick boxes hide
    Wasabi's own traffic and routine noise (the terminal's `--ignore-wasabi`
    and `--output minimal`), a snoop task filter and entry logging, find,
    pause, clear, save as text. The Amiga gives each stream to one
    listener, so the bridge holds one subscription and shares it with
    every open window; if a terminal has it, the page says so and waits.
  - **Tasks**: every task, refreshed every 3 s, stack headroom with the
    tight ones marked; Ctrl-C, or Remove by force after a warning.
  - **Run**: an AmigaDOS command with its output as it comes, the return
    code, a time limit (Ctrl-C after it) and a Stop button; recent
    commands to click again. Not checked: it does what it says.
  - **Screens**: every screen and its windows; bring one to the front.

**Reboot Amiga** sits at the foot of the sidebar, behind a confirmation.

**Its own window.** With pywebview and a system web engine installed
(`sudo pacman -S python-pywebview webkit2gtk-4.1` on this PC),
`wasabi desktop` and `wasabi view` open in a window of their own - no
tabs, no address bar, and no browser shortcuts in the way, so Ctrl+W
reaches the Amiga. Closing the window ends the program. Without them,
or with `--browser`, they open in the web browser: Chrome, Chromium or
Brave as an app window where one is on the path, else Firefox, else
the system's default browser (how the one-file downloads always open,
and macOS and Windows usually). `make
install-desktop` puts **Wasabi** in the app menu, with its icon
(`desktop/`). Tested with pywebview's Qt backend (off-screen); the GTK
one, which the command above installs, is the same pywebview call.

The app only answers its own page: requests must be addressed to it by
name (`127.0.0.1` or `localhost`, stopping DNS rebinding), anything that
changes something must carry an `X-Wasabi` header (which another web
site cannot send), and the live screen's connection must come from the
app's own origin. It serves this PC only. The service behind the pages
is `app/wasabi_api.py`; the pages are in `view/src/pages/`.

## Wasabi phone

The same app on a phone, served by an always-on machine on the Amiga's
LAN: `wasabi serve` in a Docker container. Here that is a Synology NAS,
reached over Tailscale.

**Your own:** [`tools/docker-compose.yml`](../tools/docker-compose.yml) runs
it on any Docker host (a NAS, a Raspberry Pi, a home server) with the
stock `python:3.13-alpine` image - the file says what to put where. Keep
port 8077 on your LAN or a VPN; the login is the second lock, not the
first. (`tools/nas-stack.yml` and `tools/deploy-nas.sh` are this
project's own NAS setup, kept as an example.)

- **A login.** The first visit chooses the password (at least 8
  characters; stored as a salted scrypt hash); after that each phone or
  computer logs in once a month. Tailscale keeps strangers out already;
  the password is the second lock.
- **Made for a phone.** A bottom bar instead of the sidebar; Files
  shows one side at a time (Amiga | NAS); Reboot and Log out at the foot
  of Overview. On the Screen page: tap to click, drag to drag, hold half
  a second for the right mouse button (the Amiga's menus - keep holding,
  slide, lift), and a key bar with Return, Backspace, Del, Esc, Tab, the
  arrows, Help and a keyboard for text (sent through the Amiga's own
  keymap, so å and @ come out right; "Hold Right Amiga" makes one letter
  a menu shortcut). Turn the phone sideways for a bigger screen.
- **Files and screenshots** use one folder on the NAS,
  `/volume2/docker/wasabi/files`, and nothing outside it.
- **Add to Home screen** in Chrome gives it an icon.
- **History and alerts.** The NAS reads the Amiga's health every 30 s and
  keeps a day of it: Overview gets *Live / Last hour / Last day*. It also
  watches, and tells the phone through **ntfy** (a second small
  container on the NAS, `tools/ntfy-stack.yml`, port 8090, and the free
  ntfy app): too hot (default 75 °C, set in the app), a power or heat
  problem now or newly recorded since the Pi started, not answering for
  two minutes (and back again), a new guru. The app's Alerts card lists
  them, holds the settings and sends a test. The ntfy topic is random -
  it is the feed's only key.
- **A nightly backup** of the whole boot volume, `SYS:` (03:30, 30 nights
  kept, and what to copy, all settable on the Overview's Backup card,
  with *Back up now*)
  into `Backups/Amiga/<date>/` in the NAS folder. A file unchanged since
  the night before is a hard link to that night's copy, so thirty
  nights cost little more than one. Measured on the A1200: all of SYS:,
  1,815 files and 27.7 MB, in 13.4 s; the next night 1.2 s, nothing new,
  33 MB for both. A drawer link looping back up stops at 24 levels. A
  file that cannot be read is skipped and named; a failed night is an
  alert. Restoring is the Files page: open the night
  on the NAS side and copy the file back.
- **Updates apply themselves.** `tools/deploy-nas.sh` copies the program
  and then writes `.deployed` last; Wasabi on the NAS sees the stamp
  change, ends, and Docker's restart policy starts it on the new code
  (~20 s). Open pages see the new version and reload. No Portainer
  restart after an update any more.

Setting it up: `tools/deploy-nas.sh` copies the program to
`/volume2/docker/wasabi/app` (and, the first time, the Amiga's key to
the private `config` folder - the NAS's shares give new files to
everyone, so the script takes that away), and writes the Portainer
stack to `/volume2/docker/_portainer-stacks/wasabi.yml`
(`tools/nas-stack.yml`): `python:3.13-alpine` running the program from
that folder, on the NAS's own network so it finds the Amiga by
broadcast, as the NAS user. In Portainer: Stacks > Add stack > name
`wasabi` > paste the file > Deploy. After an update: run the script
again, then restart the container in Portainer.

## Health

```
$ wasabi health
Pi (Raspberry Pi Compute Module 4 Rev 1.1): 42.8 °C (limit 85), core 1.01 V, ARM 2000 MHz
power: OK - no under-voltage or throttling since boot
Emu68 1.1.0-alpha.2 (28.07.2026) git:e23e328: Pi up 53 h 15 min, 68k 0.3 MIPS (ARM 53), JIT cache 30% used, 427849 misses
memory: chip 2010 KB free (largest block 2010 KB), fast 1837 MB free (largest 909 MB)
```

The **power line is the one to watch.** The Pi's firmware remembers
whether under-voltage, a capped clock, throttling or the soft
temperature limit happened *at any moment since the Pi booted*, so a
supply dip too short for any poll to catch still shows up. A problem
happening right now makes `health` exit 1. "Pi up" is the Pi's own
uptime: an Amiga reboot does not reset it, so neither does it reset the
since-boot flags.

The **68k MIPS** figure is a real CPU meter: Emu68 counts every 68k
instruction it executes, and an idle Amiga sits in `Wait()` with the
CPU genuinely stopped, so idle reads near 0 and a busy loop read
1287 MIPS on the A1200. It needs two readings, so a one-shot `health`
takes a second; `--watch` (every 2 s, or `--watch 1`) prints a reading
each time until Ctrl-C. `--json` gives numbers; `--raw` the daemon's
lines. On FS-UAE or a plain 68000 the Pi and Emu68 lines say *not
available* and the memory line still works. How each value is read is
in [PROTOCOL.md](../PROTOCOL.md).

## Starting programs, and the clipboard

`wasabi run --detach "SYS:Utilities/Clock"` starts a program and returns
at once - right for anything with a window, which never ends by itself
and would hold the one run slot (no more `Run >NIL:` in front). Its
output goes to a file in `T:` named on stderr. Plain `run` keeps waiting,
as before. A command's errors and its output come as one stream: the
separate error channel for commands (`SYS_Error`) is new in AmigaOS 4,
not in 3.2, and 3.x programs print their errors to normal output anyway.

`wasabi clip get` prints the Amiga's clipboard; `wasabi clip set TEXT`
(or `-` / nothing for stdin) puts text there, as IFF FTXT - the format
every Amiga program reads. Proven both ways on the A1200: a clip set
from the PC pasted into a Shell with Right Amiga+V, and text selected and
copied in a Shell came back to the PC.
