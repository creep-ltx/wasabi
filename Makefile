# wasabid - cross-built with Bebbo's m68k-amigaos-gcc.
# The toolchain lives under $(HOME)/opt/amiga on this box, not
# /opt/amiga; override CC if yours is elsewhere.

CC      = $(HOME)/opt/amiga/bin/m68k-amigaos-gcc
# -Wno-pointer-sign: every string in the NDK is 'unsigned char *', so
# passing an ordinary C literal to Printf/SystemTags warns on every call.
# It is noise, not a finding, and it drowns real warnings if left on.
CFLAGS  = -O2 -noixemul -fomit-frame-pointer -Wall -Wno-pointer-sign
DEPLOY  = /home/creep/Documents/FS-UAE/Hard Drives/Dump/Code

all: wasabid

wasabid: wasabid.c
	$(CC) $(CFLAGS) wasabid.c -o wasabid

# Round-trip the client against the host mock - no Amiga in the loop.
test:
	./tests/run-tests.sh

deploy: wasabid
	cp wasabid "$(DEPLOY)/"

clean:
	rm -f wasabid

.PHONY: all test deploy clean
