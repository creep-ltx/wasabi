#!/usr/bin/env bash
# The client against Zenix's real daemon: its core (net/wasabi/ in a
# Zenix checkout) built for this PC, as Zenix's own tools/host/
# wasabi-test.sh builds it - the same code as /sbin/wasabid, over Linux's
# calls, with ten pretend consoles. No emulator, no Amiga.
#
#   WASABI_ZENIX=/path/to/zenix tests/zenix-tests.sh
#
# run-tests.sh runs it when WASABI_ZENIX is set. Needs a C compiler and
# python3; nothing is written to the Zenix checkout.
set -u
cd "$(dirname "$0")/.."
command -v timeout >/dev/null 2>&1 || timeout() { gtimeout "$@"; }

ZX=${WASABI_ZENIX:?name a Zenix checkout: WASABI_ZENIX=/path/to/zenix}
for f in tools/host/wasabi-host.c net/wasabi/wasabi.h user/regex.c; do
    [ -f "$ZX/$f" ] || { echo "zenix-tests: $ZX has no $f - not a Zenix checkout with wasabid's core"; exit 2; }
done

T=$(mktemp -d /tmp/wasabi-zenix.XXXXXX)
export XDG_CACHE_HOME="$T/xdg-cache" XDG_CONFIG_HOME="$T/xdg-config"
KEY=zenix-test-key-0123456789
OFF=$((20000 + $$ % 20000))
ZP=$((1236 + OFF))
R="$T/root"
PASS=0
FAIL=0
DPID=
cleanup() {
    [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT
ok() { PASS=$((PASS+1)); printf '  ok    %s\n' "$1"; }
no() { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$1"; }
check() { # name expected actual
    if [ "$2" = "$3" ]; then ok "$1"; else
        no "$1"; printf '        expected: %s\n        actual:   %s\n' "$2" "$3"
    fi
}

# --- build the core, as Zenix's wasabi-test.sh does (regex.c beside a
# small libz.h of the C library's) ---
mkdir -p "$T/shim"
cp "$ZX/user/regex.c" "$ZX/user/regex.h" "$T/shim/"
printf '#include <stdlib.h>\n#include <string.h>\n' >"$T/shim/libz.h"
SRC=""
for f in core files info pure run screen; do SRC="$SRC $ZX/net/wasabi/$f.c"; done
cc -std=gnu99 -O1 -c "$T/shim/regex.c" -o "$T/shim/regex.o" &&
    cc -std=gnu99 -O1 -o "$T/wasabi-host" "$ZX/tools/host/wasabi-host.c" $SRC "$T/shim/regex.o" ||
    { echo "zenix-tests: the core does not build"; exit 1; }

# --- a pretend Zenix root (as Zenix's wasabi-check.py sets one up) ---
mkdir -p "$R/etc" "$R/var/lib" "$R/tmp" "$R/bin" "$R/sbin" "$R/dev" "$R/data"
printf 'key %s\nname testbox\n' "$KEY" >"$R/etc/wasabid.conf"
chmod 600 "$R/etc/wasabid.conf"
printf 'ZEXE....' >"$R/sbin/wasabid"
printf '#!/bin/sh\necho "$@" > .reboot-args\necho "reboot: no room for the boot note" >&2\nexit 1\n' >"$R/bin/reboot"
chmod 755 "$R/bin/reboot"
printf 'cpu: 68030 (attnflags 7)\npi: not running under Emu68 (no device tree)\n' >"$R/.klog"
echo hello >"$R/data/a.txt"

(cd "$R" && exec "$T/wasabi-host" --root "$R" --offset "$OFF") >"$T/daemon.log" 2>&1 &
DPID=$!
python3 - "$ZP" <<'PY' || { echo "zenix-tests: the daemon did not start:"; cat "$T/daemon.log"; exit 1; }
import socket, sys, time
end = time.time() + 20
while time.time() < end:
    try:
        socket.create_connection(("127.0.0.1", int(sys.argv[1])), 0.5).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.1)
sys.exit(1)
PY

W="./wasabi --host 127.0.0.1 --port $ZP --key $KEY"
echo "wasabi client vs Zenix's wasabid core"

out=$($W ping 2>&1 | grep -c "^Zenix ")
check "ping: the banner is Zenix's" "1" "$out"
out=$($W info 2>&1 | grep "^can:" | tr ' ' '\n' | grep -c "^text$\|^rebootto$\|^zenix$")
check "info: caps text, rebootto, zenix" "3" "$out"

# TEXT
out=$($W text 2>&1 | head -1 | grep -c "^Zenix .* tty1$")
check "text: the shown console's first line" "1" "$out"
out=$($W text --json --cells 2>&1 | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["rows"], d["cols"], d["console"], d["shown"], len(d["styles"]), len(d["colours"][0]))')
check "text --json --cells: the grid's size, cells" "32 80 1 True 32 160" "$out"
$W key type 'echo hello-wasabi
' >/dev/null 2>&1
out=$($W wait text '^hello-wasabi$' --timeout 5 2>&1); rc=$?
check "key type, then wait text sees the answer" "0 1" "$rc $(echo "$out" | grep -c 'appeared after')"
out=$($W wait text 'never-there' --timeout 1 2>&1); rc=$?
check "wait text: a timeout is 124" "124" "$rc"
out=$($W text --console 3 2>&1); rc=$?
check "text of a console never used: an error" "1 1" "$rc $(echo "$out" | grep -c 'never used')"
out=$($W text --console 2 2>&1 | grep -c "login:")
check "text --console 2" "1" "$out"

# files, with Unix modes
head -c 100000 /dev/urandom >"$T/big"
$W put "$T/big" /data/big >/dev/null 2>&1 && $W get /data/big "$T/back" >/dev/null 2>&1
if cmp -s "$T/big" "$T/back"; then ok "put/get 100 KB round trip"; else no "put/get 100 KB round trip"; fi
mode() { python3 -c 'import os,sys; print("%o" % (os.stat(sys.argv[1]).st_mode & 0o7777))' "$1"; }
check "put: a new file gets 644" "644" "$(mode "$R/data/big")"
$W put --mode 750 "$T/big" /data/m >/dev/null 2>&1
check "put --mode 750" "750" "$(mode "$R/data/m")"
printf 'ZEXE....' >"$T/prog"
$W put "$T/prog" /bin/prog >/dev/null 2>&1
check "put: a Zenix program gets 755, with no --force for /bin" "755" "$(mode "$R/bin/prog")"
out=$($W ls 2>&1 | grep -c " data$")
check "ls with no path lists /" "1" "$out"

# RUN, PS, KILL
out=$($W run 'echo hi; exit 3' 2>&1); rc=$?
check "run: output and the exit code" "3 hi" "$rc $out"
out=$($W ps 2>/dev/null | head -1 | tr -s ' ')
check "ps: the pid column" " PID PRI STATE NAME" "$out"
out=$($W ps --json 2>/dev/null | python3 -c 'import json,sys; print([p["pid"] for p in json.load(sys.stdin) if p["name"]=="init"])')
check "ps --json: the pid" "[2]" "$out"
echo "77 2 sleeper" >"$R/.procs"
$W kill 77 >/dev/null 2>&1
$W kill --force sleeper >/dev/null 2>&1
check "kill by pid (SIGINT) and by name (--force: SIGKILL)" "77 2|77 9|" "$(tr '\n' '|' <"$R/.kills" 2>/dev/null)"
rm -f "$R/.procs"

# HEALTH, GRAB
out=$($W health 2>&1); rc=$?
check "health without mem.*.largest" "0 1 1" "$rc $(echo "$out" | grep -c '^Zenix .*: up ') $(echo "$out" | grep -c '^memory: chip .* free, fast .* free$')"
out=$($W grab "$T/shot.png" 2>&1); rc=$?
check "grab: the console as a picture" "0 1" "$rc $(echo "$out" | grep -c '^640x256')"

# REBOOT: what boots next, the guard, the ERR after the OK
out=$($W reboot --yes --to amigaos 2>&1); rc=$?
check "reboot --to amigaos: /bin/reboot amigaos, and its failure heard" "1 amigaos 1" \
      "$rc $(cat "$R/.reboot-args" 2>/dev/null) $(echo "$out" | grep -c 'did not happen: reboot: no room for the boot note')"
sleep 0.5
$W reboot --yes --to zenix >/dev/null 2>&1
check "reboot --to zenix" "zenix" "$(cat "$R/.reboot-args" 2>/dev/null)"
sleep 0.5
echo "77 2 fsck.zfs" >"$R/.procs"
rm -f "$R/.reboot-args"
out=$($W reboot --yes 2>&1)
check "the guard names what runs, and --force" "1 no" \
      "$(echo "$out" | grep -c 'fsck.zfs is running.*(wasabi reboot --force)') $([ -f "$R/.reboot-args" ] && echo yes || echo no)"
$W reboot --yes --force >/dev/null 2>&1
check "--force skips the guard" "yes" "$([ -f "$R/.reboot-args" ] && echo yes || echo no)"
rm -f "$R/.procs"
sleep 0.5

# wait up across the two systems' ports (here: a dead one and Zenix's)
out=$(python3 - "$ZP" "$KEY" <<'PY'
import importlib.machinery, importlib.util, sys, io, contextlib
l = importlib.machinery.SourceFileLoader("w", "wasabi")
s = importlib.util.spec_from_loader("w", l)
w = importlib.util.module_from_spec(s); l.exec_module(w)
zenix, key = int(sys.argv[1]), sys.argv[2]
w.SYSTEM_PORTS = (zenix - 1, zenix)
class A:
    host, port, json, key, timeout = "127.0.0.1", zenix - 1, False, key, 4
out = io.StringIO()
with contextlib.redirect_stdout(out):
    w.cmd_wait_up(A(), "zenix")
print(out.getvalue().strip().endswith("(port %d)" % zenix))
PY
)
check "wait up finds Zenix on the other port" "True" "$out"

# a key per machine, and a port known to be plain
mkdir -p "$XDG_CONFIG_HOME/wasabi"
printf 'key = wrong-key\nkey@127.0.0.1:%d = %s\n' "$ZP" "$KEY" >"$XDG_CONFIG_HOME/wasabi/config"
out=$(./wasabi --host 127.0.0.1 --port "$ZP" ping 2>&1 | grep -c "^Zenix ")
check "key@ADDRESS:PORT reaches Zenix with its own key" "1" "$out"
rm -f "$XDG_CONFIG_HOME/wasabi/config"
./wasabi keys init >/dev/null 2>&1
out=$(WASABI_TLS=always $W ping 2>&1)
check "with keys: Zenix closes the TLS try, plain follows, said once" "1 1" \
      "$(echo "$out" | grep -c '^Zenix ') $(echo "$out" | grep -c 'has no encryption')"
out=$(WASABI_TLS=always $W ping 2>&1)
check "the next command: plain at once, nothing said" "1 0" \
      "$(echo "$out" | grep -c '^Zenix ') $(echo "$out" | grep -c 'has no encryption')"

# The MCP server's text tool, against the real daemon
out=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"amiga_text","arguments":{}}}' |
    WASABI_HOST=127.0.0.1 WASABI_PORT=$ZP WASABI_KEY=$KEY python3 wasabi_mcp.py |
    python3 -c 'import json,sys; r=json.loads(sys.stdin.readline())["result"]; print(r["isError"], r["content"][0]["text"].splitlines()[0].startswith("Zenix"))')
check "MCP amiga_text" "False True" "$out"

if grep -q "runtime error\|AddressSanitizer" "$T/daemon.log"; then
    no "the daemon's log is clean"; cat "$T/daemon.log"
fi
echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
