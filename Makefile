# wasabid - cross-built with Bebbo's m68k-amigaos-gcc.
# Bebbo's m68k-amigaos-gcc: set AMIGA to where it is installed, otherwise it is
# looked for in the usual places (/opt/amiga, its own default, then ~/opt/amiga).
AMIGA   ?= $(firstword $(wildcard /opt/amiga $(HOME)/opt/amiga))
CC      = $(if $(AMIGA),$(AMIGA)/bin/)m68k-amigaos-gcc
# -Wno-pointer-sign: every string in the NDK is 'unsigned char *', so
# passing an ordinary C literal to Printf/SystemTags warns on every call.
# It is noise, not a finding, and it drowns real warnings if left on.
# -fbbb=-: bebbo's own backend pass off. It miscompiles loops over small
# structs - "cmp.l (a0)+" and then the same element used as if a0 had not
# moved (Knowledge/amiga/toolchain-and-testing.md). In 0.5b1 it made
# posted_forget() step 18 bytes through 14-byte entries and write -1 over
# the rexxsyslib base; the first AREXX then crashed the machine.
CFLAGS  = -Os -fbbb=- -noixemul -fomit-frame-pointer -Wall -Wno-pointer-sign -Iinclude
# Where `make deploy` copies wasabid (a folder the Amiga or emulator sees,
# e.g. an FS-UAE hard drive folder): make deploy DEPLOY=/path/to/folder
DEPLOY  ?=

all: wasabid

# patches.c is every SetFunction hook in the daemon - the debug stream,
# the snoop trace and the guru report. It is a separate translation unit
# because the rules that govern code running in someone else's task are
# not the daemon's rules, and the two must not blur together.
SRCS = wasabid.c patches.c health.c

# TLS on AmiSSL 5: tls.c alone sees its headers (they clash with the
# daemon's), and links the stubs. The SDK comes from third_party/fetch.sh.
SSL    = third_party/AmiSSL/Developer
SSLINC = -I$(SSL)/include
SSLLIB = -L$(SSL)/lib/AmigaOS3 -lamisslstubs

tls.o: tls.c tls.h $(SSL)
	$(CC) $(CFLAGS) $(SSLINC) -c tls.c -o tls.o

wasabid: $(SRCS) patches.h health.h tls.h tls.o
	$(CC) $(CFLAGS) $(SRCS) tls.o $(SSLLIB) -o wasabid

# The same daemon for a 68020 or better (a stock A1200, any accelerator):
# the compiler may use the 020's instructions. wasabid itself runs on
# every 68k; this one is a little smaller and faster where it can run.
wasabid.020: $(SRCS) patches.h health.h tls.h tls.c $(SSL)
	$(CC) $(CFLAGS) -m68020 $(SSLINC) -c tls.c -o tls.020.o
	$(CC) $(CFLAGS) -m68020 $(SRCS) tls.020.o $(SSLLIB) -o wasabid.020

both: wasabid wasabid.020

$(SSL):
	third_party/fetch.sh

# Round-trip the client against the host mock - no Amiga in the loop.
test:
	./tests/run-tests.sh

deploy: wasabid
	@test -n "$(DEPLOY)" || { echo "Set DEPLOY: make deploy DEPLOY=/path/to/amiga/folder"; exit 1; }
	cp wasabid "$(DEPLOY)/"

clean:
	rm -f wasabid wasabid.020 tls.o tls.020.o

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
