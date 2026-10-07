# wasabid - cross-built with Bebbo's m68k-amigaos-gcc.
# The toolchain lives under $(HOME)/opt/amiga on this box, not
# /opt/amiga; override CC if yours is elsewhere.

CC      = $(HOME)/opt/amiga/bin/m68k-amigaos-gcc
# -Wno-pointer-sign: every string in the NDK is 'unsigned char *', so
# passing an ordinary C literal to Printf/SystemTags warns on every call.
# It is noise, not a finding, and it drowns real warnings if left on.
CFLAGS  = -O2 -noixemul -fomit-frame-pointer -Wall -Wno-pointer-sign -Iinclude
DEPLOY  = /home/creep/Documents/FS-UAE/Hard Drives/Dump/Code

all: wasabid

# patches.c is every SetFunction hook in the daemon - the debug stream,
# the snoop trace and the guru report. It is a separate translation unit
# because the rules that govern code running in someone else's task are
# not the daemon's rules, and the two must not blur together.
SRCS = wasabid.c patches.c health.c

wasabid: $(SRCS) patches.h health.h
	$(CC) $(CFLAGS) $(SRCS) -o wasabid

# Round-trip the client against the host mock - no Amiga in the loop.
test:
	./tests/run-tests.sh

deploy: wasabid
	cp wasabid "$(DEPLOY)/"

clean:
	rm -f wasabid

# The launcher entry and icon, for this user: `wasabi desktop` from the
# app menu. Exec gets the full path - a launcher's PATH may lack ~/.local/bin.
APPS  = $(HOME)/.local/share/applications
ICONS = $(HOME)/.local/share/icons/hicolor/scalable/apps
install-desktop:
	mkdir -p "$(APPS)" "$(ICONS)"
	cp desktop/wasabi.svg "$(ICONS)/wasabi.svg"
	sed "s|^Exec=wasabi|Exec=$(HOME)/.local/bin/wasabi|" desktop/wasabi.desktop \
	    > "$(APPS)/wasabi.desktop"
	-update-desktop-database "$(APPS)" 2>/dev/null

.PHONY: all test deploy clean install-desktop
