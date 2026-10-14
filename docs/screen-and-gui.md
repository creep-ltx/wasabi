# The screen and the GUI

[Back to the README](../README.md)

## Seeing the screen

`grab` is the verb, `screen` is the thing you operate on.

`wasabi grab shot.png` grabs the frontmost screen and writes a PNG
locally; with no file name each grab gets a new file in
`$TMPDIR/wasabi-grab/`, so nothing is overwritten. The path is printed
on stdout, the details on stderr. 1280x960 in **0.1 seconds**, which is
a design decision rather than luck:

```
$ wasabi grab shot.png
1280x960 -> shot.png (1362 KB, 3.7 MB raw in 0.1 s)
shot.png
$ wasabi grab --window "GrabTest"
window "GrabTest", 400x120 -> /tmp/wasabi-grab/grab-20261006-192848-516.png (1 KB, 3.7 MB raw in 0.2 s)
/tmp/wasabi-grab/grab-20261006-192848-516.png
```

`--window TITLE` crops to the frontmost window whose title contains
TITLE, using the same window list as `wasabi windows`; it must be on the
front screen (`wasabi screen --to-front` brings it). `--screen NAME`
grabs a public screen that is not in front instead; the two do not
combine.

**The Amiga sends raw pixels and does no compression at all.** Every
measurement in this file says the same thing — the wire is the fastest
part of this machine. 109 MB/s on the network against 20 MB/s from disk,
20 MB/s of ChaCha20, and a CPU that is the bottleneck behind every slow
thing here. A 1280x960 screen is 3.7 MB, which is thirty milliseconds of
wire and far less than the Amiga would spend deflating it. So the Amiga
reads its framebuffer and stops; the Linux box, idle and fast, makes the
PNG. Doing the "wasteful" thing is several times quicker end to end than
a screen grabber that compresses on the Amiga first.

**It grabs the frontmost screen, whatever that is**, and picks how to
read it from the screen's own depth — because no single call covers
both cases:

| Screen | Method | Measured |
|---|---|---|
| deeper than 8 bits | CyberGraphX `ReadPixelArray()`, true RGB | 1280x960 in **0.1 s** |
| 8 bits or fewer | `ReadPixelArray8()` + `GetRGB32()` palette | 640x256 in **2.0 s** |

CyberGraphX's autodoc says plainly that `ReadPixelArray` "should only be
used on screens depths > 8 bits", which rules it out for every native
Amiga screen — AGA stops at 8 bitplanes. So below that the pixels come
from `graphics.library` as pen numbers and the screen's palette turns
them into RGB. That path needs no RTG stack at all, so it also covers a
stock machine with no Picasso96.

The native path is far slower per byte — `ReadPixelArray8()` does a
planar-to-chunky conversion on the CPU, and the CPU is this machine's
slow part — but a PAL screen is a fifth the size, so it still lands in
about two seconds.

**Native screens come out in 4:3, not squished.** A non-interlaced
640x256 fills a whole monitor, so its pixels are twice as wide as they
are tall — dumped one-to-one it looks squashed to half height. The
client doubles the rows until the shape is plausible again (640x256
becomes 640x512; superhires 1280x256 doubles twice to 1280x1024),
which is safe by construction: no square-pixel mode is 2:1 or wider,
so an RTG screen is never touched. The wire always carries the pixels
exactly as read — the doubling happens where the PNG is made — and
`--raw` turns it off:

```
$ wasabi grab shot.png
640x256 (rows doubled to 640x512) -> shot.png (14 KB, 0.5 MB raw in 2.0 s)
```

(Lores-interlace — 320x512, pixels twice as *tall* as wide — would want
the columns doubled instead. The client leaves it alone: it cannot see
the screen's mode, only its shape, and a portrait RTG screen deserves
not to be mangled for resembling one.)

```
$ wasabi screen
ADDR       SIZE        DEPTH  TITLE
0x09265ee8 640x256         2  CygnusEd Professional V4.2   <- front
0x0829b3e0 1280x960       24  Workbench Screen

$ wasabi screen --cycle                 # exactly what Amiga+M does
$ wasabi screen --to-front "Workbench Screen"
```

Flipping screens is `ScreenToBack()`/`ScreenToFront()`, not synthesised
keystrokes into `input.device` — naming a screen beats cycling blindly
through them, and it keeps wasabi out of the business of injecting input.

One race is worth stating: the front screen is read through
`LockIBase()`, which makes reading the pointer safe but does not stop
the screen closing afterwards. Grabbing a screen that is being closed
during those few milliseconds would read freed memory. A *named* public
screen is properly locked and has no such window.

The PNG encoder on the client is about fifteen lines around `zlib`,
which is in the standard library — so this costs no dependency.

### What changed on screen

`--diff` compares a grab against an earlier one and reports what moved:

```
$ wasabi grab before.png
$ wasabi run "MyApp"
$ wasabi grab after.png --diff before.png
1280x960 -> after.png (204 KB, 3.7 MB raw in 0.4 s)
diff vs before.png: 18432 of 1228800 pixels changed (1.50%), bounding box 320,180 - 447,323
```

The exit status follows `diff(1)` — 0 identical, 1 changed — so "did that
command touch the screen" is a shell conditional. The comparison is
entirely client-side and the wire is unchanged. A baseline of a different
size, or a PNG outside the 8-bit RGB subset this client writes, is
refused rather than diffed against whatever happens to line up.

### Coordinates

A native 640x256 screen has pixels twice as tall as they are wide, so
`grab` doubles its rows to keep the picture's shape (640x512). By
default every position Wasabi shows or takes is in that picture's
pixels: `windows`, `ui list`, `mouse click`, `mouse where` - what you see
in the PNG is where you click. RTG screens have square pixels and are
not doubled.

Scripts written against the screen's own pixels (Intuition's, and
amiagent's) can ask for those instead: `--native`, `coords = native` in
`~/.config/wasabi/config`, or `WASABI_COORDS=native`. Then `grab` keeps
640x256, positions are reported in screen pixels, and `mouse` takes
them, doubling y on the way to the daemon itself.

`grab --region X,Y,W,H` takes one rectangle, in whichever pixels are in
use; `mouse where` says where the pointer is without sending a picture.

### Keys

`key press` names: `return`, `enter`, `esc`, `tab`, `del`, `backspace`,
`help`, `space`, the arrows, `f1`-`f12`, `home`, `end`, `pageup`,
`pagedown`, `insert`, the keypad (`kp0`-`kp9`, `kpdot`, `kpenter`,
`kpplus`, `kpminus`, `kpstar`, `kpslash`, `kplparen`, `kprparen`),
letters, digits, and any raw key code as `0x45`; with modifiers
(`shift`, `ctrl`, `alt`, `lamiga`, `ramiga`) joined by `+`. The PC-keyboard
keys (F11, F12, Home, End, Page Up/Down, Insert) use the codes OS
3.2/3.9 and AROS give them. The `WASABI` ARexx port knows the same
names inside `<...>`.

## The view

`wasabi view` opens the Amiga's screen in a window on the PC, live, and
your mouse and keyboard work the Amiga - like a VNC viewer. Click the
picture to give it the keyboard. Close the window and `wasabi view`
ends too (`--stay` keeps it serving).

**Keys go by position.** The PC sends *which key* was pressed, and the
Amiga's own keymap decides the character - so with a Swedish keymap on
the Amiga, the key right of L types ö, exactly as at its real keyboard.
The PC has no Amiga keys, so **Settings** (top right) chooses which PC
key plays Left Amiga, Right Amiga, Ctrl, Left Alt and Right Alt: any of
Left/Right Ctrl, Left/Right Alt, Left/Right Super (Windows key) or the
Menu key. A PC key plays one Amiga key at a time; giving it to another
takes it from the first. The defaults: Right Ctrl → Right Amiga (the
menu-shortcut key), Menu key → Left Amiga, Left Ctrl → Ctrl, and the
Alts as themselves (Right Alt is AltGr, which the Amiga's keymap needs
for @ { [ and the rest). Insert plays Help. Settings are kept in
`~/.config/wasabi/view.json`, so every browser shares them.

Things a browser keeps for itself: the desktop may take the Super keys
before the page sees them, and a few Ctrl shortcuts (Ctrl+W, Ctrl+T)
belong to the browser - **Full screen** lets the page keep more.

**How it works.** A small bridge in `app/wasabi_view.py` serves the page
(built from `view/src` into `view/dist`, which is committed, so no Node
is needed to run it) and talks to the daemon over one connection per
page: first every input the page sent, then one `LIVE` frame. The daemon
remembers what that page was last sent and replies with only the
rectangles that changed, in a compact format (16-bit colour for
graphics-card screens, pen numbers and palette for native ones); the
bridge passes them straight to the page. A still screen costs 8 bytes a
frame instead of 3.7 MB. Measured over the Wi-Fi hop: a key press shows
up on the page **52 ms** later, every time — full grabs took 90-245 ms
(median 164). The bridge looks 20 times a second while you are using it
or the screen is changing, and 5 times a second when all is still: each
look costs the Amiga ~20 ms of work, so an open, idle view takes ~5% of
its CPU. Against an older daemon without `LIVE` it falls back to whole
grabs. `wasabi grab` is unchanged and still exact. The **mouse wheel**
works the NewMouse way: each notch is raw key `$7A` (up) / `$7B` (down),
which MultiView, Workbench drawers and MUI lists scroll to (proven in
MultiView on the A1200; `wasabi mouse wheel down 3` - or `wasabi key
press wheeldown` - does the same from the command line). The Amiga's own
pointer is a sprite and is not in the picture - the PC's pointer stands
in for it. Ordinary keys go as whole presses (down and
up together) and the PC's own key repeat repeats them; only the
modifiers go down and up separately. input.device repeats a written key
until its up arrives, so an up that arrives late - it happened once,
through an ordering bug since fixed - makes the Amiga type "eeeeeee". A Chromium-family
browser opens it as an app window with no tabs or address bar; Firefox
has no such mode, so it gets a new window (`--kiosk`: full screen).
`--no-browser` just prints the address. It serves this PC only; the
phone will reach it through the NAS (step 5 of the plan).

## Driving the GUI

`windows`, `mouse` and `key` together drive the Amiga's desktop from
here — look, click, type — for tests or to help when nobody is at the
keyboard.

```
$ wasabi windows
screen "Workbench Screen"  1280x960, 24 bit
  window "WasabiKeyTest"  at 100,100  640x200  task -  [active]
  window ""  at 0,18  1280x942  task Workbench  [backdrop]
$ wasabi key type 'echo "Hej på dig!"' --enter
$ wasabi key press up return          # the Shell's history: run it again
$ wasabi key press ramiga+e           # Workbench's Execute Command
```

**`windows`** lists every screen front first, each followed by its
windows front first. Positions are relative to the screen — the same
pixels `grab` shows and `mouse` clicks — so a window is found and clicked
from facts, not by guessing pixels in a picture. On a native
non-interlaced screen `grab` doubles the rows, so `windows` doubles y and
heights too and the numbers still match the picture. `task` is the program
reading the window (its command name if it came from a Shell); a Shell
window shows `-`, because console windows are read by the console
device, not by a program. `--raw` prints the daemon's tab-separated lines,
in the screen's own unscaled pixels, (the format is in [PROTOCOL.md](../PROTOCOL.md)). The list is
made under Intuition's lock and sent after the lock is dropped, so a
slow network never holds the Amiga's screen still.

**`key type`** sends characters, and the Amiga's own keymap works out
which keys make them (`MapANSI()`). So the PC needs no idea of the
Amiga's layout: on a Swedish keymap å, ä, ö, @ and `{[]}` all came out
byte for byte. The whole text is checked before the first key goes out,
so a character the keymap cannot make fails the command instead of
leaving half a line typed. Newlines become Return.

**`key press`** sends key *positions* by name, the way the keyboard
does: `return`, `esc`, `tab`, `del`, `backspace`, `help`, `up`/`down`/
`left`/`right`, `f1`–`f10`, letters, digits and the US-position
punctuation, with modifiers joined by `+` (`shift`, `ctrl`, `alt`,
`lamiga`, `ramiga`/`amiga`, and the left/right variants). Modifiers go
down first and up last, as fingers do it.

Keys go to whichever window is active, exactly like the real keyboard.
`windows` shows which one that is; click a window first to give it the
keys. A dialog that has no text field selected ignores typing, and not
every dialog closes on Esc — Workbench's Execute Command needs its
underlined shortcut (`key press c` for Cancel).

## Gadgets and menus by name, and ARexx

`windows` gives places; `ui` gives *things*. It lists each window's
gadgets — buttons, text fields, checkboxes, cycle gadgets, sliders,
the border gadgets — with their labels, states and boxes, and its menu
strip, all in the pixels `grab` and `mouse` use:

```
$ wasabi ui list --window gtwin
window "gtwin test"  at 120,80  300x208  task RAM:gtwin  [active]
  close    -                        at 120,80 20x22
  string   "Name"                   at 216,122 148x16  value "Hello Åsa"
  button   "Colour"                 at 210,156 160x28  value "Green"
  toggle   "Loud"                   at 210,196 26x22  [selected]
  button   "Save"                   at 210,236 100x28
  menu     Project/Open...                   Amiga+O
  menu     Project/Recent/Second file
  menu     Settings/Loud                     [checked]
  menu     Settings/Disabled one             [disabled]
$ wasabi ui set name 'Hello Åsa'      # click, Right Amiga+X, type, Return
$ wasabi ui click save
$ wasabi ui click close --window Execute
$ wasabi ui menu 'project/recent/second'
$ wasabi ui menu quit
```

Names match in any case, without the shortcut underscore and trailing
dots or colon: exact first, then the start of a name, then any part of
it. Two matches in one window are an error that lists them; `--nth`
picks one. Without `--window` the front screen is searched, front
window first.

**A click is a real click**, at the middle of the gadget, with the
same mouse `mouse` uses — so the program sees exactly what a person's
click gives it. When another window covers the spot, or the window's
screen is not in front, `ui click` brings the window forward first. A
disabled (ghosted) gadget is refused rather than clicked.

**A menu pick is not mouse work.** `ui menu` hands the window the
message Intuition sends when someone picks an item (an IDCMP MENUPICK
with the item's number). It works the same with pop-up menus or the
menu bar, in a window that is not active, and cannot slip onto the
neighbouring item. A checkmark item is ticked the way Intuition would.
Disabled items are refused.

**What it sees:** plain Intuition and GadTools windows (most of OS
3.2's Prefs editors), system requesters (their buttons are
`frbuttonclass`), the border gadgets of every window, and since 0.7:

- **ReAction** windows: the buttons, fields and other gadgets inside
  their layouts, nested layouts included. Their children live in the
  layout's own memory; the daemon finds them there, read-only, by the
  shape of the data rather than fixed offsets (see PROTOCOL.md).
- **MUI 4 and 5** windows: buttons, text fields (`ui set` works),
  cycle gadgets, sliders, lists, radio buttons and checkmarks, with
  their text. A field or checkmark without text of its own takes the
  label MUI shows before or after it. Plain text appears as `label`
  and is never what `ui click` clicks. Found by a search of memory the
  first time a window is looked at (0.4 s on a 68030/50 emulator), then
  remembered.
- **List rows (0.9):** the entries of GadTools and MUI lists, each a
  `row` with its text, whether it is selected, and where it is drawn -
  so a row is clicked by its text, like a button:

  ```
  $ wasabi ui list --window "Address Book"       # DCTelnet 1.10
    button   -                        at 120,142 401x136
        row 0    "Retro32 BBS"            at 122,146 397x16  [selected]
        row 1    "TeleHack simulation"    at 122,162 397x16
    button   "Connect"                at 129,300 93x26
  $ wasabi ui click telehack --window "Address Book" --double
  ```

  GadTools rows come from the public `GT_GetGadgetAttrs()`; MUI rows
  from the list itself (`MUIM_List_GetEntry`), placed exactly with
  `MUIM_List_TestPos`. A row scrolled out of view is listed as such and
  refused for a click. Up to 100 rows per list. An MUI list with
  several columns shows its first column (the code that draws the
  others is the program's own and not reachable from outside).

Not seen: MUI 3.8 windows (nothing tells them apart without searching
all memory for every window), custom-drawn content (games, paint
areas), and a GadTools checkbox whose label is a picture. For those,
`grab` and `mouse` as before. GadTools keeps a button's text in its own
memory; the daemon reads it there, carefully.

### The WASABI ARexx port

Amiga programs and scripts get the same hands, without a network:

```rexx
/* drive a window from ARexx */
options results
address WASABI
'ACTIVATEWINDOW "Shell#?"'        /* AmigaDOS pattern; RESULT = title */
'TYPE "dir<return>"'              /* <key> names inside, << for a < */
'KEY ramiga+q'
'CLICK 120 40'                    /* front screen pixels; RIGHT, DOUBLE */
'CLICKGADGET Save WINDOW "MyPrefs#?"'   /* NTH n for the nth match */
'MENU "Project/Quit"'             /* or just 'MENU Quit' */
'WINDOWS'; say result             /* every titled window */
'LASTERROR'; say result           /* why the last command failed */
```

`rc` is 0 done, 5 nothing matched, 10 refused (LASTERROR says why), 20
not a command. Without `WINDOW`, CLICKGADGET and MENU use the active
window. `CLICKGADGET` sees what `wasabi ui` sees, ReAction and MUI
included. Only programs on the Amiga itself reach the port, and they
could do all of this through input.device anyway.

**ARexx.** `wasabi arexx PORT COMMAND` sends one command to a
program's ARexx port and prints its RESULT; the exit code is the
command's own rc (0, 5, 10, 20). `wasabi arexx` alone lists the public
ports. Port names are case-sensitive.

```
$ wasabi arexx WORKBENCH getattr application.version
47.42
```

The daemon does not wait for the answer: the reply comes back through
its main loop, so a slow program holds up only the command that asked.
For a whole ARexx script, `wasabi run --max-time 30 'rx script.rexx'`.
