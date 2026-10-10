#!/usr/bin/env bash
# Exercise the wasabi client against the host mock daemon. No Amiga
# required - prove the client on the host before trusting it on iron.
set -u

cd "$(dirname "$0")/.."

# macOS ships no GNU timeout; Homebrew's coreutils spells it gtimeout.
command -v timeout >/dev/null 2>&1 || timeout() { gtimeout "$@"; }

PORT=${PORT:-14231}
KEY=hunter2
ROOT=$(mktemp -d /tmp/wasabi-test.XXXXXX)
# Keep every cache and config write inside the test root - the suite must
# never touch the real ~/.cache/wasabi or ~/.config/wasabi.
export XDG_CACHE_HOME="$ROOT/xdg-cache"
export XDG_CONFIG_HOME="$ROOT/xdg-config"
CACHE=$XDG_CACHE_HOME/wasabi/last-host
PASS=0
FAIL=0

cleanup() {
    [ -n "${MOCK_PID:-}" ] && kill "$MOCK_PID" 2>/dev/null
    [ -n "${MOCK2_PID:-}" ] && kill "$MOCK2_PID" 2>/dev/null
    [ -n "${MOCK4_PID:-}" ] && kill "$MOCK4_PID" 2>/dev/null
    [ -n "${MOCKE_PID:-}" ] && kill "$MOCKE_PID" 2>/dev/null
    [ -n "${VIEW_PID:-}" ] && kill "$VIEW_PID" 2>/dev/null
    rm -rf "$ROOT"
}
trap cleanup EXIT

ok() { PASS=$((PASS+1)); printf '  ok    %s\n' "$1"; }
no() { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$1"; }

check() { # name expected actual
    if [ "$2" = "$3" ]; then ok "$1"; else
        no "$1"; printf '        expected: %s\n        actual:   %s\n' "$2" "$3"
    fi
}

mkdir -p "$ROOT/C" "$ROOT/L"
echo "hello from the amiga" > "$ROOT/C/greet.txt"

./tests/mock-wasabid.py --root "$ROOT" --port "$PORT" --key "$KEY" \
    >"$ROOT/mock.log" 2>&1 &
MOCK_PID=$!
sleep 1

W="./wasabi --host 127.0.0.1 --port $PORT --key $KEY"

echo "wasabi client vs mock-wasabid"

# --- discovery (no --host: must find the mock by broadcast) ---
rm -f "$CACHE"
out=$(./wasabi --port "$PORT" discover 2>&1 | grep -c "127.0.0.1")
check "discover finds the daemon" "1" "$out"

# Two emulators on this PC, one port each: both are found.
./tests/mock-wasabid.py --root "$ROOT" --port $((PORT+1)) --key "$KEY" \
    >"$ROOT/mocke.log" 2>&1 &
MOCKE_PID=$!
sleep 1
out=$(./wasabi --port "$PORT" discover 2>&1 | grep -c "127.0.0.1")
check "discover lists each emulator on its own port" "2" "$out"
kill "$MOCKE_PID" 2>/dev/null; MOCKE_PID=

# The cached address stopped answering (the Amiga got a new one after a
# restart): the next command finds it again by itself and remembers it.
mkdir -p "$(dirname "$CACHE")"
echo "127.0.0.1 1 $(uname -n)" > "$CACHE"
out=$(./wasabi --port "$PORT" --key "$KEY" ping 2>&1 | grep -c "mock-wasabid")
check "a moved Amiga is found again without discover" "1" "$out"
check "and its new address is remembered" "127.0.0.1 $PORT $(uname -n)" "$(cat "$CACHE")"

# Answers only from loopback must not stop the subnet sweep, and the real
# machine wins over an emulator unless the emulator was the one last used.
out=$(python3 - <<'PY'
import importlib.machinery, importlib.util
l = importlib.machinery.SourceFileLoader("w", "wasabi")
s = importlib.util.spec_from_loader("w", l)
w = importlib.util.module_from_spec(s); l.exec_module(w)
sent = []
class Sock:
    def __init__(self, *a): pass
    def setsockopt(self, *a): pass
    def setblocking(self, *a): pass
    def close(self): pass
    def sendto(self, data, addr): sent.append(addr[0])
def collect(sock, found, deadline):
    if "10.9.9.2" in sent:
        found[("10.9.9.2", 1234)] = ("a1200", "")
    found[("127.0.0.1", 1234)] = ("amiga", "")
w.socket.socket = Sock
w._collect = collect
w.local_ipv4s = lambda: [("10.9.9.1", "255.255.255.252")]
found = w.discover()
emu, real = ("127.0.0.1", 1234), ("10.9.9.2", 1234)
print(real in found, w.pick_machine(found) == real,
      w.pick_machine(found, "amiga") == emu)
PY
)
check "an emulator answering does not stop the search for the A1200" \
      "True True True" "$out"

# --- handshake ---
out=$($W ping 2>&1 | grep -c "mock-wasabid")
check "ping completes the handshake" "1" "$out"

out=$(./wasabi --host 127.0.0.1 --port "$PORT" --key wrong ping 2>&1 | \
      grep -c "bad key")
check "a wrong key is refused" "1" "$out"

# --- listing ---
out=$($W ls C: 2>/dev/null | grep -c "greet.txt")
check "ls shows a file" "1" "$out"

# A DateStamp is the Amiga's wall time and must arrive verbatim - not
# shifted by this box's UTC offset (it was, before 0.1b26).
touch -t 202601021030 "$ROOT/C/dated.txt"
out=$($W ls C: 2>/dev/null | grep "dated.txt" | grep -c "2026-01-02 10:30")
check "ls dates are wall time, verbatim" "1" "$out"

# --- round trip, multi-frame ---
head -c 200000 /dev/urandom > "$ROOT/../wasabi-big.$$"
$W put --force "$ROOT/../wasabi-big.$$" L:big >/dev/null 2>&1
$W get L:big "$ROOT/../wasabi-back.$$" >/dev/null 2>&1
if cmp -s "$ROOT/../wasabi-big.$$" "$ROOT/../wasabi-back.$$"; then
    ok "200000-byte put/get round trip is byte-identical"
else
    no "200000-byte put/get round trip is byte-identical"
fi
rm -f "$ROOT/../wasabi-big.$$" "$ROOT/../wasabi-back.$$"

# --- put is atomic: no temp file left behind ---
# tr strips the padding BSD wc puts in front of its number.
out=$(find "$ROOT" -name "*.wasabi-tmp" | wc -l | tr -d ' \t')
check "put leaves no temp file behind" "0" "$out"

# --- run: output and exit code ---
out=$($W run "echo one; echo two" 2>/dev/null | tr '\n' ',')
check "run streams stdout in order" "one,two," "$out"

$W run "exit 7" >/dev/null 2>&1
check "run propagates the exit code" "7" "$?"

out=$($W run "echo to-stderr >&2" 2>&1 >/dev/null)
check "run keeps stderr separate" "to-stderr" "$out"

# wasabid holds a command in 512 bytes and strncpy truncates silently;
# the client must refuse rather than let half a command run.
out=$($W run "$(python3 -c 'print("echo " + "x" * 600)')" 2>&1 | grep -c "511")
check "run refuses a command wasabid would truncate" "1" "$out"

# --- run slots (wasabid 0.4): several commands at once ---
$W run "sleep 2; echo first" > "$ROOT/slot1.out" 2>&1 &
S1=$!
sleep 0.5
out=$($W run "echo second" 2>&1)
check "a second command runs while the first is still going" "second" "$out"
out=$($W slots 2>&1 | grep -c "sleep 2; echo first")
check "slots shows the running command" "1" "$out"
wait $S1
check "and the first one still finishes" "first" "$(cat "$ROOT/slot1.out")"

FOUR=
for i in 1 2 3 4; do $W run "sleep 3" >/dev/null 2>&1 & FOUR="$FOUR $!"; done
sleep 0.7
out=$($W run "echo fifth" 2>&1 | grep -c "all run slots are busy")
check "a fifth command is refused while four run" "1" "$out"
wait $FOUR

$W run "sleep 20" > "$ROOT/slotf.out" 2>&1 &
S1=$!
sleep 0.5
N=$($W --json slots | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["slot"])')
out=$($W free "$N" 2>&1 | grep -c "stopped")
check "free stops a slot's command with Ctrl-C" "1" "$out"
for i in $(seq 1 20); do kill -0 $S1 2>/dev/null || break; sleep 0.25; done
kill -0 $S1 2>/dev/null; check "and its run ends" "1" "$?"

$W run "sleep 6 # stubborn" > "$ROOT/slots.out" 2>&1 &
S1=$!
sleep 0.5
N=$($W --json slots | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["slot"])')
out=$($W free "$N" 2>&1 | grep -c "free again")
check "free lets go of a command that ignores Ctrl-C" "1" "$out"
for i in $(seq 1 20); do kill -0 $S1 2>/dev/null || break; sleep 0.25; done
out=$(grep -c "let go of" "$ROOT/slots.out")
check "and its run is told so" "1" "$out"
out=$($W free 7 2>&1 | grep -c "no command in that slot")
check "free on an empty slot says so" "1" "$out"

# --- mkdir / del ---
$W mkdir L:newdrawer >/dev/null 2>&1
check "mkdir creates a drawer" "0" "$?"
[ -d "$ROOT/L/newdrawer" ] && ok "the drawer really exists" \
                           || no "the drawer really exists"

$W del --force L:big >/dev/null 2>&1
[ ! -f "$ROOT/L/big" ] && ok "del removes a file" || no "del removes a file"

# --- streams ---
# TIMING CONTRACT: the mock emits one SNOOP_SAMPLES line per 0.35 s idle
# tick, so a sample's position in that list is how long a test must wait
# to see it - roughly 0.35 s x index, plus interpreter start-up. Every
# timeout below must clear that with room for a CI runner slower than a
# dev box. Adding samples ahead of a tested line pushes it later and can
# break a green suite from a distance: that is exactly how the macOS job
# went red on 13 Aug 2026, while ubuntu stayed green.
out=$(timeout -s INT 3 $W snoop --task cfile 2>/dev/null | \
      grep -c "^cfile")
if [ "$out" -ge 2 ]; then ok "snoop honours the task filter"
else no "snoop honours the task filter (got $out lines)"; fi

out=$(timeout -s INT 3 $W snoop --task cfile 2>/dev/null | grep -c "Shell")
check "snoop filter suppresses other tasks" "0" "$out"

out=$(timeout -s INT 3 $W snoop 2>/dev/null | \
      grep -c "(Error 232: No more entries in directory)")
if [ "$out" -ge 1 ]; then ok "the live stream dresses err codes"
else no "the live stream dresses err codes"; fi

ERRLOG=$ROOT/err.log
timeout -s INT 3 $W snoop --log "$ERRLOG" >/dev/null 2>&1
out=$(grep -c "(Error 232: No more entries in directory)" "$ERRLOG")
if [ "$out" -ge 1 ]; then ok "and so does the stream's log file"
else no "and so does the stream's log file"; fi
out=$(grep -c "(err 232)" "$ERRLOG")
check "the terse wire form reaches neither" "0" "$out"

out=$($W debug --entry 2>&1 | grep -c "only affects the snoop trace")
check "--entry on a plain debug is refused, not ignored" "1" "$out"
out=$($W debug --task foo 2>&1 | grep -c "only affects the snoop trace")
check "and so is --task" "1" "$out"
out=$(timeout -s INT 3 $W debug --with-snoop --entry 2>/dev/null | \
      grep -c ') \.\.\.$')
if [ "$out" -ge 1 ]; then ok "but both are accepted with --with-snoop"
else no "but both are accepted with --with-snoop"; fi

out=$(timeout -s INT 3 $W debug 2>/dev/null | \
      grep -c "ALERT #80000004 (CPU: illegal instruction)")
if [ "$out" -ge 1 ]; then ok "a guru's alert code is decoded by name"
else no "a guru's alert code is decoded by name"; fi

out=$(timeout -s INT 3 $W snoop --entry 2>/dev/null | grep -c ') \.\.\.$')
if [ "$out" -ge 1 ]; then ok "entry mode shows calls on the way in"
else no "entry mode shows calls on the way in"; fi

out=$(timeout -s INT 3 $W snoop 2>/dev/null | grep -c ') \.\.\.$')
check "and only when it is asked for" "0" "$out"

out=$(timeout -s INT 5 $W snoop 2>/dev/null | grep -c "more identical")
if [ "$out" -ge 1 ]; then ok "runs of identical lines are folded"
else no "runs of identical lines are folded"; fi

out=$(timeout -s INT 5 $W snoop --output full 2>/dev/null | \
      grep -c "more identical")
check "--output full folds nothing" "0" "$out"

out=$(timeout -s INT 5 $W snoop --output minimal 2>/dev/null | \
      grep -c 'OpenLibrary("dos.library"')
check "--output minimal hides the poll noise" "0" "$out"

out=$(timeout -s INT 5 $W snoop --output minimal 2>/dev/null | \
      grep -c "Startup-Sequence")
if [ "$out" -ge 1 ]; then ok "and keeps the lines that matter"
else no "and keeps the lines that matter"; fi

out=$(timeout -s INT 6 $W snoop --ignore-wasabi 2>/dev/null | \
      grep -cE "^(wasabid|c:wasabid|wasabi-runner)")
check "--ignore-wasabi hides the tool's own traffic" "0" "$out"

# the runner's temp file, whoever touches it
out=$(timeout -s INT 6 $W snoop --ignore-wasabi 2>/dev/null | \
      grep -c "T:wasabi-run-")
check "and its temp files, whichever task opens them" "0" "$out"

# the other half of the contract: what it must NEVER hide
out=$(timeout -s INT 5 $W debug --ignore-wasabi 2>/dev/null | \
      grep -c "ALERT #80000004")
if [ "$out" -ge 1 ]; then ok "but never the guru report"
else no "but never the guru report"; fi

out=$(timeout -s INT 6 $W snoop --ignore-wasabi --output full 2>/dev/null | \
      grep -cE "^(wasabid|c:wasabid|wasabi-runner)")
check "and still hides it with --output full" "0" "$out"

out=$(timeout -s INT 6 $W snoop --ignore-wasabi 2>/dev/null | \
      grep -c "Startup-Sequence")
if [ "$out" -ge 1 ]; then ok "while keeping what is being debugged"
else no "while keeping what is being debugged"; fi

out=$(timeout -s INT 3 $W debug 2>/dev/null | grep -c "ReadCacheNode")
if [ "$out" -ge 1 ]; then ok "debug streams lines"
else no "debug streams lines"; fi

# --- combined view and --log ---
STREAMLOG=$ROOT/../wasabi-streamlog.$$
out=$(timeout -s INT 3 $W debug --with-snoop --log "$STREAMLOG" 2>/dev/null)
d=$(printf '%s\n' "$out" | grep -c "^debug | ")
s=$(printf '%s\n' "$out" | grep -c "^snoop | ")
if [ "$d" -ge 1 ] && [ "$s" -ge 1 ]; then ok "combined view carries both streams"
else no "combined view carries both streams (debug $d, snoop $s)"; fi

n=$(grep -cE \
    '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} (debug|snoop) \| ' \
    "$STREAMLOG" 2>/dev/null)
if [ "${n:-0}" -ge 2 ]; then ok "--log stamps every line"
else no "--log stamps every line (got ${n:-0})"; fi

# The mock sends the daemon's empty heartbeat LOGs; neither the view
# nor the log file may show them as blank lines.
h=$(printf '%s\n' "$out" | grep -cE '^(debug|snoop) \| $')
check "heartbeats are invisible in the stream view" "0" "$h"
h=$(grep -cE ' (debug|snoop) \| $' "$STREAMLOG" 2>/dev/null)
check "and invisible in the log file" "0" "${h:-0}"

# The log must say for itself how the stream was started and how it
# ended - a reader should never have to assume what was subscribed.
c=$(grep -cE ' client \| debug\+snoop stream open to 127\.0\.0\.1' \
    "$STREAMLOG" 2>/dev/null)
check "--log records how the stream was started" "1" "${c:-0}"
c=$(grep -cE ' client \| debug\+snoop stream closed' "$STREAMLOG" 2>/dev/null)
check "and that it was closed" "1" "${c:-0}"
rm -f "$STREAMLOG"

# --- stream reconnect: a mock that hangs up on its subscriber once ---
# The stream must announce the loss, come back by itself, keep flowing,
# and not mistake the daemon's restarted sequence numbers for loss.
PORT2=$((PORT+1))
./tests/mock-wasabid.py --root "$ROOT" --port "$PORT2" --key "$KEY" \
    --drop-stream-after 2 >"$ROOT/mock2.log" 2>&1 &
MOCK2_PID=$!
sleep 1
W2="./wasabi --host 127.0.0.1 --port $PORT2 --key $KEY"

RECERR=$ROOT/reconnect.err
out=$(timeout -s INT 8 $W2 debug 2>"$RECERR" | grep -c "ReadCacheNode")
lost=$(grep -c "connection lost" "$RECERR")
back=$(grep -c "reconnected after" "$RECERR")
check "a dropped stream announces the loss" "1" "$lost"
check "and reconnects by itself" "1" "$back"
if [ "$out" -ge 3 ]; then ok "and the stream flows again"
else no "and the stream flows again (got $out lines)"; fi
gap=$(grep -c "frame(s) lost" "$RECERR")
check "a reconnect is not mistaken for lost frames" "0" "$gap"
kill $MOCK2_PID 2>/dev/null; MOCK2_PID=

# --once restores stop-on-disconnect: the client must exit on its own,
# well before the timeout would have killed it.
PORT2=$((PORT+2))
./tests/mock-wasabid.py --root "$ROOT" --port "$PORT2" --key "$KEY" \
    --drop-stream-after 2 >"$ROOT/mock3.log" 2>&1 &
MOCK2_PID=$!
sleep 1
W2="./wasabi --host 127.0.0.1 --port $PORT2 --key $KEY"
timeout 8 $W2 debug --once >"$ROOT/once.out" 2>&1
check "--once exits when the connection drops" "0" "$?"
out=$(grep -c "stream closed" "$ROOT/once.out")
check "and says the stream closed" "1" "$out"
kill $MOCK2_PID 2>/dev/null; MOCK2_PID=

# --- deploy --restart: upload then reload the daemon in one shot ---
echo restarter > "$ROOT/../wasabi-restart.$$"
out=$($W deploy --force "$ROOT/../wasabi-restart.$$" C:wasabid.new --restart 2>&1 | \
      grep -c "reloading itself")
check "deploy --restart uploads and reloads" "1" "$out"
rm -f "$ROOT/../wasabi-restart.$$"

# --- the guru frame decoder ---------------------------------------
# Host-compiled against frames built by hand from M68000PM Appendix B.
# The decoder source is extracted from patches.c at test time, so this
# cannot drift from the code it is checking. The Amiga cannot test this
# itself: Emu68 does not deliver CPU exceptions to the guest, so a real
# frame never reaches the hook there.
if command -v gcc >/dev/null 2>&1; then
    awk '/^static ULONG guru_be32/,/^}/'      patches.c >  "$ROOT/dec.inc"
    awk '/^static UWORD guru_be16/,/^}/'      patches.c >> "$ROOT/dec.inc"
    awk '/^static BOOL guru_find_frame/,/^\}$/' patches.c >> "$ROOT/dec.inc"
    if gcc -O2 -I"$ROOT" -o "$ROOT/dectest" tests/dectest.c 2>/dev/null \
       && "$ROOT/dectest" >"$ROOT/dectest.out" 2>&1; then
        ok "the guru frame decoder handles every documented format"
    else
        no "the guru frame decoder handles every documented format"
        cat "$ROOT/dectest.out" 2>/dev/null | tail -5
    fi
else
    ok "guru frame decoder (skipped: no host gcc)"
fi

# --- update: the daemon may only be replaced through verification ---
NEWD=$ROOT/../wasabi-newd.$$
printf 'binary\0$VER: wasabid 9.9test (1.1.2026)\0rest\n' > "$NEWD"
printf 'old daemon\n' > "$ROOT/C/wasabid"

out=$($W put --force "$NEWD" C:wasabid 2>&1 | grep -c "wasabi update")
check "put refuses to overwrite the running daemon" "1" "$out"
check "and leaves it alone" "old daemon" "$(cat "$ROOT/C/wasabid")"

# LICENSE is a local file guaranteed to exist and to carry no $VER tag
# (/etc/hostname, the old choice, does not exist on macOS).
out=$($W update LICENSE 2>&1 | grep -c "not a wasabid binary")
check "update refuses a file with no \$VER tag" "1" "$out"
check "still leaves the daemon alone" "old daemon" "$(cat "$ROOT/C/wasabid")"

$W update "$NEWD" --testport $((PORT+1)) >/dev/null 2>&1
if cmp -s "$NEWD" "$ROOT/C/wasabid"; then
    ok "update installs a binary that passes every check"
else
    no "update installs a binary that passes every check"
fi
check "update keeps the previous binary" \
      "old daemon" "$(cat "$ROOT/C/wasabid.bak" 2>/dev/null)"
[ ! -f "$ROOT/C/wasabid.new" ] && ok "update clears the sidecar" \
                              || no "update clears the sidecar"

# --no-trial: the flag must actually skip the trial daemon, not just
# exist. It shipped once as a no-op, which is worth a test of its own.
printf 'old daemon\n' > "$ROOT/C/wasabid"
out=$($W update "$NEWD" --no-trial 2>&1)
check "--no-trial says it skipped the live probe" "1" \
      "$(printf '%s\n' "$out" | grep -c 'skipped (--no-trial)')"
check "--no-trial does not start a trial daemon" "0" \
      "$(printf '%s\n' "$out" | grep -c 'served a handshake')"
if cmp -s "$NEWD" "$ROOT/C/wasabid"; then
    ok "--no-trial still installs the binary"
else
    no "--no-trial still installs the binary"
fi

# A real wasabid that passes its own self-test and still cannot serve:
# only the live probe can catch this one.
DEADD=$ROOT/../wasabi-deadd.$$
printf 'BREAK_SERVE\0$VER: wasabid 9.9dead (1.1.2026)\0x\n' > "$DEADD"
out=$($W update "$DEADD" --testport $((PORT+2)) 2>&1 | grep -c "did not come up as a daemon")
check "update catches a binary that cannot serve" "1" "$out"
if cmp -s "$NEWD" "$ROOT/C/wasabid"; then
    ok "and the working daemon is still in place"
else
    no "and the working daemon is still in place"
fi
[ ! -f "$ROOT/C/wasabid.new" ] && ok "and its sidecar is cleared" \
                              || no "and its sidecar is cleared"
rm -f "$NEWD" "$DEADD"

# --- speedtest ---
out=$($W speedtest 1MB --pings 20 2>/dev/null | grep -c "MB/s")
check "speedtest reports both directions" "2" "$out"

out=$($W speedtest 1MB --pings 20 2>/dev/null | grep -c "jitter")
check "speedtest measures latency" "1" "$out"

out=$($W speedtest 999GB 2>&1 | grep -c "256 MB")
check "speedtest refuses an absurd size" "1" "$out"

# --- screen grab: raw over the wire, PNG written here ---
SHOT=$ROOT/../wasabi-shot.$$
$W grab "$SHOT" >/dev/null 2>&1
# The magic is checked in python: BSD grep will not commit to an exit
# status on bytes it considers binary.
if [ -s "$SHOT" ] && python3 -c "
import sys
sys.exit(0 if open('$SHOT','rb').read(8) == b'\x89PNG\r\n\x1a\n' else 1)"
then
    ok "grab writes a real PNG"
else
    no "grab writes a real PNG"
fi
# The mock's 8x4 screen is 2:1 - the shape of a native non-interlaced
# grab - so the client doubles its rows into 4:3-ish proportions.
out=$(python3 -c "
import struct,sys
d=open('$SHOT','rb').read()
print('%dx%d' % struct.unpack('>II', d[16:24]))" 2>/dev/null)
check "a squished native screen gets its rows doubled" "8x8" "$out"

$W grab --raw "$SHOT" >/dev/null 2>&1
out=$(python3 -c "
import struct,sys
d=open('$SHOT','rb').read()
print('%dx%d' % struct.unpack('>II', d[16:24]))" 2>/dev/null)
check "--raw keeps the pixels exactly as sent" "8x4" "$out"

# --- grab --diff: regression checking against an earlier shot ----------
# The mock draws the same screen every time, so two grabs must compare
# identical - which also round-trips write_png through read_png, the
# only proof that the new decoder agrees with the encoder beside it.
BASE=$ROOT/../wasabi-base.$$
$W grab "$BASE" >/dev/null 2>&1
$W grab "$SHOT" --diff "$BASE" >/dev/null 2>&1
check "an unchanged screen diffs clean, and exits 0" "0" "$?"
out=$($W grab "$SHOT" --diff "$BASE" 2>&1 >/dev/null | grep -c "identical")
check "and says so" "1" "$out"

# Doctor one pixel of the baseline: the diff must find it, locate it,
# and exit 1 the way diff(1) does so a script can branch on it.
python3 - "$BASE" <<'EOF'
import struct, sys, zlib
p = sys.argv[1]
d = open(p, "rb").read()
w, h = struct.unpack(">II", d[16:24])
pos, idat = 8, []
while pos + 8 <= len(d):
    (ln,) = struct.unpack_from(">I", d, pos)
    typ = d[pos+4:pos+8]
    if typ == b"IDAT":
        idat.append(d[pos+8:pos+8+ln])
    pos += 12 + ln
raw = bytearray(zlib.decompress(b"".join(idat)))
raw[1 + 3] ^= 0xFF                      # row 0, filter byte, then pixel 1
def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(
        ">I", zlib.crc32(t + b) & 0xFFFFFFFF)
open(p, "wb").write(
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    + chunk(b"IEND", b""))
EOF
$W grab "$SHOT" --diff "$BASE" >/dev/null 2>&1
check "a changed screen exits 1" "1" "$?"
out=$($W grab "$SHOT" --diff "$BASE" 2>&1 >/dev/null | \
      grep -c "1 of 64 pixels changed")
check "and counts exactly the pixels that moved" "1" "$out"
out=$($W grab "$SHOT" --diff "$BASE" 2>&1 >/dev/null | \
      grep -c "bounding box 1,0 - 1,0")
check "and locates it" "1" "$out"

# A baseline from a different screen mode is refused, not diffed against
# whatever happens to line up.
$W grab --raw "$ROOT/../wasabi-raw.$$" >/dev/null 2>&1
out=$($W grab "$SHOT" --diff "$ROOT/../wasabi-raw.$$" 2>&1 >/dev/null | \
      grep -c "same screen mode")
check "a baseline of a different size is refused" "1" "$out"
rm -f "$BASE" "$ROOT/../wasabi-raw.$$"
rm -f "$SHOT"

out=$($W screen 2>/dev/null | grep -c "CygnusEd Professional V4.2")
check "screen lists what is open" "1" "$out"
out=$($W screen 2>/dev/null | grep -c "<- front")
check "and marks the front one" "1" "$out"

out=$($W screen --to-front NoSuchScreen 2>&1 | grep -c "no screen with that title")
check "screen --to-front of a missing title errors" "1" "$out"

# --- keyboard and the window list ---
# The mock logs each KEY payload as hex: u16 mode, then raw (code,
# qualifier) pairs or Latin-1 text.
rm -f "$ROOT/keys.log"
$W key press ramiga+w >/dev/null 2>&1
out=$(tail -1 "$ROOT/keys.log" 2>/dev/null)
check "key press sends modifier down, key, key up, modifier up" \
      "0000006700800011008000910080""00e70000" "$out"
$W mouse wheel down 2 >/dev/null 2>&1
out=$(tail -1 "$ROOT/keys.log" 2>/dev/null)
check "mouse wheel down 2 sends the NewMouse key twice" \
      "0000007b000000fb0000007b000000fb0000" "$out"
out=$($W mouse wheel sideways 2>&1 | grep -c "up or down")
check "mouse wheel refuses a direction it cannot turn" "1" "$out"
out=$($W mouse move ten 20 2>&1 | grep -c "must be a number")
check "mouse move still refuses a coordinate that is not a number" "1" "$out"
$W key type 'å' --enter >/dev/null 2>&1
out=$(tail -1 "$ROOT/keys.log" 2>/dev/null)
check "key type sends Latin-1 text, Return as CR" "0001e50d" "$out"
out=$($W key press nosuchkey 2>&1 | grep -c "unknown key")
check "an unknown key name is refused here" "1" "$out"

# The mock's screen is 640x256, which grab shows with its rows doubled -
# so the window list doubles y and height to match the picture.
out=$($W windows 2>/dev/null | grep -c 'window "AmigaShell"  at 100,200  640x400  task -  \[active\]')
check "windows lists a window in the picture's pixels" "1" "$out"
out=$($W windows --raw 2>/dev/null | grep -c "AmigaShell" )
check "and --raw keeps the daemon's own numbers" "1" "$out"
out=$($W windows --raw 2>/dev/null | grep "AmigaShell" | cut -f4)
check "unscaled" "100" "$out"
out=$($W windows --raw 2>/dev/null | grep -c "^W")
check "windows --raw passes the daemon's lines through" "2" "$out"

# --- ps / kill ---
out=$($W ps 2>/dev/null | grep -c "input.device")
check "ps lists tasks" "1" "$out"

out=$($W ps 'input#?' 2>/dev/null | grep -c "device")
check "ps honours a filter" "1" "$out"

# --- ps stack headroom -------------------------------------------------
# Capacity says what a task was given; headroom says how close it is to
# the crash. Four things have to hold: the column appears, an
# unmeasurable task says so rather than guessing, a tight one is called
# out on stderr, and - the one that matters for anyone already running a
# client - a daemon without the capability still parses.
out=$($W ps 2>/dev/null | grep -c "FREE")
check "ps shows a stack headroom column" "1" "$out"

out=$($W ps 'input#?' 2>/dev/null | grep -cE "6144 +380")
check "and the headroom next to the capacity" "1" "$out"

out=$($W ps 'con#?' 2>/dev/null | grep -cE "4096 +-")
check "a task whose stack cannot be measured says so" "1" "$out"

out=$($W ps 2>&1 >/dev/null | grep -c "low stack - input.device has 380 of 6144")
check "a task close to the edge is called out" "1" "$out"

# The warning is proportional, not a flat byte count. A 512-byte device
# task at ~300 free is 59% clear and must NOT be flagged - a flat 1024
# threshold warned about three idle system tasks on every real ps.
out=$($W ps 2>&1 >/dev/null | grep -c "con_handler")
check "a small stack with room to spare is not flagged" "0" "$out"

# A daemon from before the flags word: the client must not ask, and must
# still read the seven-field line rather than dropping every row.
CAPS_NOFREE="ping,info,ls,put,get,run,del,mkdir,debug,snoop,reboot,restart,ps,kill,speed,speedfile,quit,install,grab,screen,hb,guru,snoopentry"
./tests/mock-wasabid.py --root "$ROOT" --port $((PORT+4)) --key "$KEY" \
    --caps "$CAPS_NOFREE" >"$ROOT/mock-nofree.log" 2>&1 &
MOCK4_PID=$!
sleep 1
WOLD="./wasabi --host 127.0.0.1 --port $((PORT+4)) --key $KEY"
out=$($WOLD ps 2>/dev/null | grep -c "input.device")
check "a daemon without 'psfree' still lists tasks" "1" "$out"
out=$($WOLD ps 2>/dev/null | grep -c "FREE")
check "and is not asked for a column it lacks" "0" "$out"
out=$($WOLD key press return 2>&1 | grep -c "update it")
check "a daemon without 'key' is named as too old" "1" "$out"
out=$($WOLD health 2>&1 | grep -c "update it")
check "a daemon without 'health' is named as too old" "1" "$out"
out=$($WOLD clip get 2>&1 | grep -c "update it")
check "a daemon without 'clip' is named as too old" "1" "$out"
kill "$MOCK4_PID" 2>/dev/null

$W kill Wait >/dev/null 2>&1
check "kill by command name succeeds" "0" "$?"

out=$($W kill nosuchtask 2>&1 | grep -c "no task")
check "kill of a missing task errors" "1" "$out"

out=$($W kill con_handler 2>&1 | grep -c "2 tasks match con_handler - name one by its address")
check "kill of an ambiguous name errors, naming the addresses" "1" "$out"
out=$($W kill clock 2>&1 | grep -c "sent Ctrl-C to clock (0x08052400)")
check "kill finds a program by its bare name, path or not" "1" "$out"
out=$($W ps clock 2>/dev/null | grep -c "SYS:Utilities/Clock")
check "and so does ps" "1" "$out"

out=$($W kill wasabid 2>&1 | grep -c "restart or reboot")
check "kill refuses the daemon itself" "1" "$out"

# --- capabilities in WELCOME ---
out=$($W info 2>/dev/null | grep -c "^can: .*speed")
check "info reports what the daemon can do" "1" "$out"

out=$($W info 2>/dev/null | grep -cE "^  [A-Za-z]+: +[0-9]+ MB total +[0-9]+ MB free")
check "info lists volumes with size and free" "1" "$out"

# A daemon too old for ps: the client should name it, not send blindly.
./tests/mock-wasabid.py --root "$ROOT" --port $((PORT+7)) --key "$KEY" \
    --caps "ping,info,ls,put,get,run" >>"$ROOT/mock.log" 2>&1 &
OLD_PID=$!
sleep 1
out=$(./wasabi --host 127.0.0.1 --port $((PORT+7)) --key "$KEY" ps 2>&1 | \
      grep -c "has no 'ps'")
check "a daemon without a capability is named, not guessed at" "1" "$out"
out=$(./wasabi --host 127.0.0.1 --port $((PORT+7)) --key "$KEY" ping 2>&1 | \
      grep -c "mock-wasabid")
check "commands it does have still work" "1" "$out"
kill $OLD_PID 2>/dev/null

# One that snoops but predates entry logging: the specific gap is named,
# not the whole command refused.
./tests/mock-wasabid.py --root "$ROOT" --port $((PORT+9)) --key "$KEY" \
    --caps "ping,info,run,debug,snoop,hb" >>"$ROOT/mock.log" 2>&1 &
OLD_PID=$!
sleep 1
out=$(./wasabi --host 127.0.0.1 --port $((PORT+9)) --key "$KEY" \
      snoop --entry 2>&1 | grep -c "no entry logging")
check "and entry logging it lacks is named too" "1" "$out"
kill $OLD_PID 2>/dev/null

# A daemon from before capabilities existed: never refuse, just try.
./tests/mock-wasabid.py --root "$ROOT" --port $((PORT+8)) --key "$KEY" \
    --caps "" >>"$ROOT/mock.log" 2>&1 &
PRE_PID=$!
sleep 1
out=$(./wasabi --host 127.0.0.1 --port $((PORT+8)) --key "$KEY" ps 2>&1 | \
      grep -c "input.device")
check "a pre-capability daemon is not second-guessed" "1" "$out"
kill $PRE_PID 2>/dev/null

# --- refused off-LAN connections, reported to the operator ---
rm -f "$XDG_CACHE_HOME/wasabi/refused-127.0.0.1"
./tests/mock-wasabid.py --root "$ROOT" --port $((PORT+9)) --key "$KEY" \
    --refused 673 >>"$ROOT/mock.log" 2>&1 &
REF_PID=$!
sleep 1
R="./wasabi --host 127.0.0.1 --port $((PORT+9)) --key $KEY"
out=$($R ping 2>&1 | grep -c "673 connection(s) refused")
check "a rising refusal count is reported once" "1" "$out"
out=$($R ping 2>&1 | grep -c "refused as off-LAN")
check "and not repeated when it has not moved" "0" "$out"
out=$($R info 2>/dev/null | grep -c "^refused: 673")
check "info always shows the total" "1" "$out"
kill $REF_PID 2>/dev/null
rm -f "$XDG_CACHE_HOME/wasabi/refused-127.0.0.1"

# --- step 2: made for scripts - limits, wait, look, JSON, safety ---

# The safety catch: system places need --force, and nothing is sent
# without it.
echo "precious" > "$ROOT/C/precious"
out=$($W del C:precious 2>&1 | grep -c "system place")
check "del in C: without --force is refused" "1" "$out"
check "and the file is untouched" "precious" "$(cat "$ROOT/C/precious")"
out=$($W put LICENSE S:Startup-Sequence 2>&1 | grep -c "system place")
check "put into S: without --force is refused" "1" "$out"
[ ! -f "$ROOT/S/Startup-Sequence" ] && ok "and nothing was written" \
                                     || no "and nothing was written"
out=$($W put LICENSE Work:x >/dev/null 2>&1; echo $?)
check "an ordinary place needs no --force" "0" "$out"
mkdir -p "$XDG_CONFIG_HOME/wasabi"
echo "protect = Work" > "$XDG_CONFIG_HOME/wasabi/config"
out=$($W del Work:x 2>&1 | grep -c "system place")
check "a 'protect =' volume in the config is guarded too" "1" "$out"
rm -f "$XDG_CONFIG_HOME/wasabi/config"
out=$($W reboot 2>&1 | grep -c "Pass --yes")
check "reboot needs --yes" "1" "$out"

# run --max-time: the mock knows no task called 'sleep', so the Ctrl-C
# cannot land - the limit must still end the wait, with exit 124.
s0=$(date +%s)
$W run --max-time 1 "sleep 8" >/dev/null 2>&1
rc=$?
check "run --max-time ends a long command with exit 124" "124" "$rc"
check "and does so on time" "1" "$(( $(date +%s) - s0 < 5 ))"

out=$($W --json run "echo hi")
check "run --json gives the output and the code" \
      '{"rc": 0, "output": "hi\n"}' "$out"

# A frozen daemon: stopped, it still has a listening socket, so the
# connection is taken and then nothing answers. That is exit 125, not
# "cannot reach" (2) and never a hang.
kill -STOP $MOCK_PID
$W --silence 2 ping >/dev/null 2>&1
rc=$?
kill -CONT $MOCK_PID
check "a daemon that takes the connection but never answers is 125" "125" "$rc"

# ...and one that freezes in the middle of a run.
( sleep 1.5; kill -STOP $MOCK_PID ) &
WASABI_RUN_CHECK=1 $W --silence 2 run "sleep 20" >/dev/null 2>&1
rc=$?
kill -CONT $MOCK_PID
check "an Amiga that freezes during run is noticed: 125" "125" "$rc"
sleep 0.5

# wait
out=$($W wait window amigashell --timeout 3 2>/dev/null | grep -c "appeared")
check "wait window finds an open window" "1" "$out"
$W wait window NoSuchWindow --timeout 1 >/dev/null 2>&1
check "wait gives up with exit 124" "124" "$?"
$W wait window amigashell --gone --timeout 1 >/dev/null 2>&1
check "wait --gone on a window that stays open times out" "124" "$?"
out=$($W wait file C:greet.txt --contains HELLO --timeout 3 2>/dev/null \
      | grep -c "hello from the amiga")
check "wait file --contains matches any case and shows the line" "1" "$out"
( sleep 1; echo "late line" > "$ROOT/T-late.txt" ) &
$W wait file T-late.txt --timeout 5 >/dev/null 2>&1
check "wait file sees a file that turns up later" "0" "$?"
out=$($W --json wait task 'input#?' --timeout 3)
check "wait task, as JSON" "1" "$(echo "$out" | grep -c '"met": true')"
$W wait up --timeout 3 >/dev/null 2>&1
check "wait up returns at once when the Amiga answers" "0" "$?"

# grab: a new file each time when none is named, the path on stdout,
# one window by title
out=$(TMPDIR="$ROOT" $W grab 2>/dev/null)
case "$out" in "$ROOT"/wasabi-grab/grab-*.png) ok "grab with no file picks a new one and prints it";;
    *) no "grab with no file picks a new one and prints it"; echo "        got: $out";; esac
[ -s "$out" ] && ok "and the picture is there" || no "and the picture is there"
out=$($W grab "$ROOT/named.png" 2>/dev/null)
check "grab FILE prints that path" "$ROOT/named.png" "$out"
out=$($W grab --window nosuch 2>&1 | grep -c "no window")
check "grab --window on a missing window says so" "1" "$out"
out=$($W grab --window amigashell 2>&1 | grep -c "off the screen")
check "a window outside the picture is refused, not cut to nothing" "1" "$out"
out=$($W --json grab "$ROOT/j.png" 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["path"], d["width"])')
check "grab --json gives the path and size" "$ROOT/j.png 8" "$out"
out=$($W grab "$ROOT/j2.png" --json 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["path"], d["width"])')
check "--json also works after the command" "$ROOT/j2.png 8" "$out"

# JSON
out=$($W --json ping | python3 -c 'import json,sys; print("key" in json.load(sys.stdin)["caps"])')
check "ping --json carries the caps" "True" "$out"
out=$($W --json ls C: | python3 -c 'import json,sys; print(any(e["name"]=="greet.txt" for e in json.load(sys.stdin)))')
check "ls --json lists the files" "True" "$out"

# Streams that end by themselves
s0=$(date +%s)
$W debug --for 1 >/dev/null 2>&1
rc=$?
check "debug --for ends by itself with exit 0" "0" "$rc"
check "and on time" "1" "$(( $(date +%s) - s0 < 4 ))"
out=$($W snoop --until 'startup-sequence' --for 10 2>/dev/null | grep -c "Startup-Sequence")
check "snoop --until stops at the line it waited for" "1" "$out"
$W debug --until 'never said' --for 1 >/dev/null 2>&1
check "--until that never comes is exit 124" "124" "$?"

# --- health ---
out=$($W --json health 2>/dev/null)
check "health reads the temperature" "51.5" "$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["temp_c"])')"
check "and works out the CPU meter from two readings" "1" \
      "$(echo "$out" | python3 -c 'import json,sys; m=json.load(sys.stdin)["mips_68k"]; print(int(90 < m < 110))')"
check "and names what happened since boot" "under-voltage,throttled" \
      "$(echo "$out" | python3 -c 'import json,sys; print(",".join(json.load(sys.stdin)["problems_since_boot"]))')"
out=$($W health 2>/dev/null | grep -c "since boot there was under-voltage, throttled")
check "health says so in words" "1" "$out"
$W health >/dev/null 2>&1
check "a problem in the past only is exit 0" "0" "$?"

# --- view: the bridge between a page and the Amiga ---
# A raw WebSocket client plays the page: it must get a status line and
# the first picture, and a key it sends must reach the daemon as one
# whole press, Right Amiga's bit set while Right Amiga is held. With a
# daemon that has LIVE the picture is its compact head + rect; without
# (WASABI_VIEW_NOLIVE plays one) it is whole RGB rows from GRAB.
cat > "$ROOT/page.py" <<'PY'
import base64, json, os, socket, struct, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n"
           % (sys.argv[1], base64.b64encode(os.urandom(16)).decode())).encode())
head = b""
while b"\r\n\r\n" not in head:
    head += s.recv(1)
def read(n):
    b = b""
    while len(b) < n:
        b += s.recv(n - len(b))
    return b
def frame():
    b0, b1 = read(2); n = b1 & 0x7F
    if n == 126: (n,) = struct.unpack(">H", read(2))
    elif n == 127: (n,) = struct.unpack(">Q", read(8))
    return b0 & 0x0F, read(n)
def send(obj):
    d = json.dumps(obj).encode(); mask = os.urandom(4)
    s.sendall(bytes([0x81, 0x80 | len(d)]) + mask +
              bytes(c ^ mask[i & 3] for i, c in enumerate(d)))
# like the real app: "this window is open", for as long as it is
hello = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
hello.sendall(("GET /ws/hello HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n"
               % (sys.argv[1], base64.b64encode(os.urandom(16)).decode())).encode())
got = []
want = 3
heads = 0
while len(got) < want:
    op, d = frame()
    if op == 1 and b'"connected": true' in d:
        got.append("status")
    elif op == 2 and d[0] == 1:
        _, w, h, y, rows = struct.unpack(">BHHHH", d[:9])
        got.append("band %dx%d" % (w, h)); want = 2
    elif op == 2 and d[0] == 0x10:
        heads += 1
        if heads == 1:
            got.append("head %dx%d fmt %d full %d" % struct.unpack(">HHBB", d[1:7]))
    elif op == 2 and d[0] == 0x12:
        x, y, w, h = struct.unpack(">HHHH", d[1:9])
        got.append("rect %dx%d %d bytes" % (w, h, len(d) - 9))
print(", ".join(got))
# after the first frame, an unchanged screen costs only heads
quiet = 0
t_end = time.time() + 1.0
s.settimeout(0.2)
while time.time() < t_end:
    try:
        op, d = frame()
    except socket.timeout:
        continue
    if op == 2 and d[0] == 0x12:
        quiet += 1
print("rects while still: %d" % quiet)
send({"t": "key", "code": 0x67, "down": True})
send({"t": "key", "code": 0x12, "down": True})
send({"t": "key", "code": 0x12, "down": False})
send({"t": "key", "code": 0x67, "down": False})
time.sleep(1)
PY
VPORT=$((PORT+7))
./wasabi --host 127.0.0.1 --port $PORT --key $KEY view --no-browser \
    --port $VPORT >"$ROOT/view.log" 2>&1 &
VIEW_PID=$!
sleep 1.5
rm -f "$ROOT/keys.log"
out=$(timeout 10 python3 "$ROOT/page.py" "$VPORT")
check "view: the page gets a status line and the first LIVE picture" \
      "status, head 8x4 fmt 1 full 1, rect 8x4 64 bytes" "$(echo "$out" | head -1)"
check "view: a still screen sends no pixels after that" \
      "rects while still: 0" "$(echo "$out" | sed -n 2p)"
check "view: keys reach the daemon - Amiga held, E as one press, Amiga let go" \
      "000000670080 00000012008000920080 000000e70000" \
      "$(tr '\n' ' ' < "$ROOT/keys.log" | sed 's/ $//')"
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 $VIEW_PID 2>/dev/null || break; sleep 1; done
kill -0 $VIEW_PID 2>/dev/null; check "view: the bridge ends once its page has gone" "1" "$?"

# The window may show a page with no live screen (Overview, Files): the
# bridge must stay as long as the window's "hello" is open - it used to
# quit 5 s after the Screen page was left.
./wasabi --host 127.0.0.1 --port $PORT --key $KEY desktop --no-browser \
    --browser --port $VPORT >"$ROOT/desk2.log" 2>&1 &
VIEW_PID=$!
sleep 1.5
timeout 12 python3 - "$VPORT" <<'PY' &
import base64, os, socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(("GET /ws/hello HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
           % (sys.argv[1], base64.b64encode(os.urandom(16)).decode())).encode())
time.sleep(9)
PY
HELLO_PID=$!
sleep 8
kill -0 $VIEW_PID 2>/dev/null; check "desktop: stays while its window is open, live screen or not" "0" "$?"
wait $HELLO_PID 2>/dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 $VIEW_PID 2>/dev/null || break; sleep 1; done
kill -0 $VIEW_PID 2>/dev/null; check "desktop: and ends once the window has gone" "1" "$?"
kill $VIEW_PID 2>/dev/null
kill $VIEW_PID 2>/dev/null

WASABI_VIEW_NOLIVE=1 ./wasabi --host 127.0.0.1 --port $PORT --key $KEY \
    view --no-browser --port $VPORT >"$ROOT/view2.log" 2>&1 &
VIEW_PID=$!
sleep 1.5
out=$(timeout 10 python3 "$ROOT/page.py" "$VPORT" | head -1)
check "view: without LIVE it falls back to whole rows from GRAB" \
      "status, band 8x8" "$out"
kill $VIEW_PID 2>/dev/null

# --- desktop: the app's API, and the guards on it ---
./wasabi --host 127.0.0.1 --port $PORT --key $KEY desktop --no-browser \
    --port $VPORT >"$ROOT/desk.log" 2>&1 &
VIEW_PID=$!
sleep 1.5
A="http://127.0.0.1:$VPORT"
out=$(curl -s "$A/api/health" | python3 -c 'import json,sys; print(json.load(sys.stdin)["temp_c"])')
check "desktop: /api/health reads the machine" "51.5" "$out"
out=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$A/api/grab")
check "desktop: a change without X-Wasabi is refused" "403" "$out"
out=$(curl -s -o /dev/null -w "%{http_code}" -H "Host: evil.example:$VPORT" "$A/api/health")
check "desktop: a request for another host name is refused" "403" "$out"
out=$(curl -s -o /dev/null -w "%{http_code}" -H "Origin: http://evil.example" \
      -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Sec-WebSocket-Version: 13" \
      -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" "$A/ws")
check "desktop: a live screen for another site is refused" "403" "$out"
post() { curl -s -o "$ROOT/post.out" -w "%{http_code}" -X POST -H "X-Wasabi: 1" \
         -H "Content-Type: application/json" -d "$2" "$A$1"; }
out=$(post /api/copy/to-amiga "{\"paths\": [\"$PWD/LICENSE\"], \"dir\": \"L:\"}")
check "desktop: copying into L: asks first (428)" "428" "$out"
[ ! -f "$ROOT/L/LICENSE" ] && ok "desktop: and copies nothing until asked" \
                           || no "desktop: and copies nothing until asked"
out=$(post /api/copy/to-amiga "{\"paths\": [\"$PWD/LICENSE\"], \"dir\": \"L:\", \"force\": true}")
check "desktop: confirmed, it copies" "200" "$out"
cmp -s LICENSE "$ROOT/L/LICENSE" && ok "desktop: byte for byte" || no "desktop: byte for byte"
mkdir -p "$ROOT/back"
out=$(post /api/copy/to-pc "{\"items\": [{\"path\": \"C:greet.txt\"}], \"dir\": \"$ROOT/back\"}")
check "desktop: Amiga -> PC copies" "hello from the amiga" "$(cat "$ROOT/back/greet.txt" 2>/dev/null)"
out=$(curl -s "$A/api/amiga/ls?path=C:" | python3 -c 'import json,sys; print(any(e["name"]=="greet.txt" for e in json.load(sys.stdin)["entries"]))')
check "desktop: lists an Amiga drawer" "True" "$out"
kill $VIEW_PID 2>/dev/null

# --- machines: more than one Amiga in one app ---
./wasabi --host 127.0.0.1 --port $PORT --key $KEY desktop --no-browser \
    --stay --port $VPORT >"$ROOT/fleet.log" 2>&1 &
VIEW_PID=$!
sleep 1.5
A="http://127.0.0.1:$VPORT"
J=(-H "Content-Type: application/json" -H "X-Wasabi: 1")
out=$(curl -s "$A/api/machines" | python3 -c 'import json,sys; m=json.load(sys.stdin)["machines"]; print(len(m), m[0]["auto"], m[0]["online"])')
check "machines: the first one is found automatically, and online" "1 True True" "$out"
out=$(curl -s "${J[@]}" -X POST -d "{\"action\":\"add\",\"name\":\"Second\",\"host\":\"127.0.0.1\",\"port\":$PORT}" "$A/api/machines" | python3 -c 'import json,sys; print([m["id"] for m in json.load(sys.stdin)["machines"]])')
check "machines: one can be added by address" "['amiga', 'second']" "$out"
out=$(curl -s "$A/api/health?m=second" | python3 -c 'import json,sys; print(json.load(sys.stdin)["temp_c"])')
check "machines: ?m= picks the machine for a call" "51.5" "$out"
out=$(curl -s "${J[@]}" -X POST -d '{"action":"add","host":"bad host; rm","port":1}' "$A/api/machines")
check "machines: a nonsense address is refused" '{"error": "give the machine'"'"'s address, like 192.168.1.20 or 127.0.0.1"}' "$out"
curl -s "${J[@]}" -X POST -d '{"action":"remove","id":"second"}' "$A/api/machines" >/dev/null
out=$(curl -s "$A/api/machines" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["machines"]))')
check "machines: and removed again" "1" "$out"
kill $VIEW_PID 2>/dev/null

# --- serve: Wasabi phone's server - a login, and a fenced folder ---
mkdir -p "$ROOT/srv-files"
./wasabi --host 127.0.0.1 --port $PORT --key $KEY serve --port $VPORT \
    --files "$ROOT/srv-files" >"$ROOT/serve.log" 2>&1 &
VIEW_PID=$!
sleep 1.5
A="http://127.0.0.1:$VPORT"
J=(-H "Content-Type: application/json" -H "X-Wasabi: 1")
out=$(curl -s -o /dev/null -w "%{http_code}" "$A/api/health")
check "serve: nothing without logging in" "401" "$out"
out=$(curl -s "$A/api/auth/state")
check "serve: the first visitor is asked to choose a password" \
      '{"required": true, "setup": true, "logged_in": false, "version": ""}' "$out"
out=$(curl -s -c "$ROOT/jar" "${J[@]}" -X POST -d '{"password":"correct horse"}' "$A/api/auth/setup")
check "serve: setting it logs in" '{"ok": true}' "$out"
out=$(curl -s "${J[@]}" -X POST -d '{"password":"another one"}' "$A/api/auth/setup")
check "serve: and nobody can set it again" '{"error": "a password is already set"}' "$out"
out=$(curl -s -o /dev/null -w "%{http_code}" "${J[@]}" -X POST -d '{"password":"wrong guess"}' "$A/api/auth/login")
check "serve: a wrong password is refused" "401" "$out"
out=$(curl -s -b "$ROOT/jar" "$A/api/health" | python3 -c 'import json,sys; print(json.load(sys.stdin)["temp_c"])')
check "serve: logged in, it works" "51.5" "$out"
out=$(curl -s -b "$ROOT/jar" "$A/api/local/ls?path=/../../etc" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("error"))')
check "serve: the files folder cannot be left" "that is outside the Wasabi folder" "$out"
kill $VIEW_PID 2>/dev/null

# --- clip: the Amiga's clipboard, both ways ---
$W clip set "från PC:n @{}" >/dev/null 2>&1
check "clip set, then get, round-trips Latin-1 text" "från PC:n @{}" "$($W clip get 2>/dev/null)"
printf 'from stdin\n' | $W clip set >/dev/null 2>&1
check "clip set with no text reads stdin" "from stdin" "$($W clip get 2>/dev/null)"

# --- serve: history and alerts (the monitor), against a fake ntfy ---
NPORT=$((PORT+9))
python3 - "$NPORT" "$ROOT/ntfy.log" <<'PY' &
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        open(sys.argv[2], "ab").write(body + b"\n")
        self.send_response(200); self.end_headers(); self.wfile.write(b"{}")
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
NTFY_PID=$!
mkdir -p "$ROOT/mon-cfg/wasabi"
cat > "$ROOT/mon-cfg/wasabi/alerts.json" <<J
{"ntfy_url": "http://127.0.0.1:$NPORT", "topic": "wasabi-test", "temp_c": 50}
J
XDG_CONFIG_HOME="$ROOT/mon-cfg" WASABI_MONITOR_EVERY=1 ./wasabi --host 127.0.0.1 \
    --port $PORT --key $KEY serve --port $VPORT --files "$ROOT/srv-files" \
    >"$ROOT/mon.log" 2>&1 &
VIEW_PID=$!
sleep 3
grep -c '"title": "The Amiga.s Pi is hot: 51.5 \\u00b0C"' "$ROOT/ntfy.log" >/dev/null 2>&1
out=$(python3 -c 'import json,sys; print([json.loads(l)["title"] for l in open(sys.argv[1])])' "$ROOT/ntfy.log" 2>/dev/null)
check "monitor: over the temperature limit sends an alert" "1" \
      "$(echo "$out" | grep -c "The Amiga.s Pi is hot: 51.5 °C")"
check "monitor: with the topic and °C intact" "wasabi-test" \
      "$(head -1 "$ROOT/ntfy.log" | python3 -c 'import json,sys; print(json.load(sys.stdin)["topic"])')"
kill -STOP $MOCK_PID
sleep 9
kill -CONT $MOCK_PID
sleep 3
out=$(python3 -c 'import json,sys; print([json.loads(l)["title"] for l in open(sys.argv[1])])' "$ROOT/ntfy.log")
check "monitor: an Amiga that stops answering is reported" "1" "$(echo "$out" | grep -c "stopped answering")"
check "monitor: and so is its return" "1" "$(echo "$out" | grep -c "is back")"
printf 'ALERT #00000004 in task "test"\n' > "$ROOT/T-lastguru"
mkdir -p "$ROOT/T" && cp "$ROOT/T-lastguru" "$ROOT/T/lastguru"
sleep 3
out=$(python3 -c 'import json,sys; print([json.loads(l)["title"] for l in open(sys.argv[1])])' "$ROOT/ntfy.log")
check "monitor: a new guru is reported" "1" "$(echo "$out" | grep -c "Guru Meditation")"
curl -s -c "$ROOT/monjar" -H "Content-Type: application/json" -H "X-Wasabi: 1" \
     -X POST -d '{"password":"monitor-test"}' "http://127.0.0.1:$VPORT/api/auth/setup" >/dev/null
out=$(curl -s -b "$ROOT/monjar" "http://127.0.0.1:$VPORT/api/history?hours=1" | python3 -c '
import json,sys
d = json.load(sys.stdin)
r = d["readings"]
print(len(r) >= 5, any(x[1] == 0 for x in r), any(x[2] == 51.5 for x in r))')
check "monitor: keeps a history - readings, the silent ones too, the temperature" \
      "True True True" "$out"
out=$(curl -s -b "$ROOT/monjar" "http://127.0.0.1:$VPORT/api/alerts" | python3 -c '
import json,sys; print(len(json.load(sys.stdin)["events"]) >= 3)')
check "monitor: and the alerts as events for the app" "True" "$out"
kill $VIEW_PID $NTFY_PID 2>/dev/null

# --- serve: the nightly backup, and updates that apply themselves ---
mkdir -p "$ROOT/S" "$ROOT/bk-cfg/wasabi" "$ROOT/bk-files"
echo "c:wasabid" > "$ROOT/S/User-Startup"
echo "the startup" > "$ROOT/S/Startup-Sequence"
cat > "$ROOT/bk-cfg/wasabi/backup.json" <<J
{"folders": ["S:"]}
J
XDG_CONFIG_HOME="$ROOT/bk-cfg" ./wasabi --host 127.0.0.1 --port $PORT --key $KEY \
    serve --port $VPORT --files "$ROOT/bk-files" >"$ROOT/bk.log" 2>&1 &
VIEW_PID=$!
sleep 1.5
A="http://127.0.0.1:$VPORT"
J=(-H "Content-Type: application/json" -H "X-Wasabi: 1")
curl -s -c "$ROOT/bkjar" "${J[@]}" -X POST -d '{"password":"backup-test"}' "$A/api/auth/setup" >/dev/null
bk_run() {
  curl -s -b "$ROOT/bkjar" "${J[@]}" -X POST -d '{}' "$A/api/backup/now" >/dev/null
  for i in $(seq 1 20); do
    sleep 0.5
    r=$(curl -s -b "$ROOT/bkjar" "$A/api/backup" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["running"], (d.get("last") or {}).get("new"))')
    case "$r" in False*) echo "${r#False }"; return;; esac
  done
}
TODAY=$(date +%Y-%m-%d)
out=$(bk_run)
check "backup: copies the Amiga's S: to the NAS folder" "the startup" \
      "$(cat "$ROOT/bk-files/Backups/Amiga/$TODAY/S/Startup-Sequence" 2>/dev/null)"
mv "$ROOT/bk-files/Backups/Amiga/$TODAY" "$ROOT/bk-files/Backups/Amiga/2000-01-01"
out=$(bk_run)
check "backup: a later night stores nothing that did not change" "0" "$out"
check "backup: it links last night's copy instead" "2" \
      "$(stat -c %h "$ROOT/bk-files/Backups/Amiga/$TODAY/S/User-Startup" 2>/dev/null)"
# --- audit 4: what a broken or hostile request must not do ---
raw_req() { # raw HTTP request text -> status code
  python3 - "$VPORT" "$1" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(sys.argv[2].encode().replace(b"\\n", b"\r\n"))
try:
    print(s.recv(200).split(b" ")[1].decode())
except (socket.timeout, IndexError):
    print("no answer")
PY
}
out=$(raw_req 'POST /api/auth/login HTTP/1.1\nHost: x\nContent-Length: -5\nX-Wasabi: 1\n\n')
check "audit4: a negative Content-Length is refused before login" "400" "$out"
out=$(raw_req 'POST /api/auth/login HTTP/1.1\nHost: x\nContent-Length: 99999999999\nX-Wasabi: 1\n\n')
check "audit4: and so is a huge one" "413" "$out"
out=$(curl -s -o /dev/null -w "%{http_code}" "$A/api/settings")
check "audit4: settings need a login on the server" "401" "$out"
tok=$(awk '$6=="wasabi_session"{print $7}' "$ROOT/bkjar")
out=$(curl -s -o /dev/null -w "%{http_code}" -H "Cookie: bad\"cookie=x y; wasabi_session=$tok" "$A/api/backup")
check "audit4: another app's odd cookie does not log us out" "200" "$out"
out=$(curl -s -o /dev/null -w "%{http_code}" -b "$ROOT/bkjar" "${J[@]}" -X POST -d '{"time":"3","keep":"x"}' "$A/api/backup")
check "audit4: nonsense backup settings are refused" "400" "$out"
out=$(curl -s -b "$ROOT/bkjar" "$A/api/backup" | python3 -c 'import json,sys; s=json.load(sys.stdin)["settings"]; print(s["time"], s["keep"])')
check "audit4: and the old ones stay" "03:30 30" "$out"
out=$(curl -s -o /dev/null -w "%{http_code}" -b "$ROOT/bkjar" "${J[@]}" -X POST -d '{"temp_c":"hot"}' "$A/api/alerts")
check "audit4: nonsense alert settings are refused" "400" "$out"
out=$(curl -s -o /dev/null -w "%{http_code}" -b "$ROOT/bkjar" "${J[@]}" -X POST -d '[1,2]' "$A/api/backup")
check "audit4: a JSON list body is an answer, not a dropped connection" "400" "$out"
for bad in '/' '..' 'S:..'; do
  out=$(curl -s -o /dev/null -w "%{http_code}" -b "$ROOT/bkjar" "${J[@]}" -X POST \
        -d "{\"items\":[{\"path\":\"$bad\",\"dir\":false}],\"dir\":\"/\"}" "$A/api/copy/to-pc")
  check "audit4: copying Amiga '$bad' to the NAS cannot leave the folder" "400" "$out"
done
ln -s /tmp "$ROOT/bk-files/out-link"
out=$(curl -s -o /dev/null -w "%{http_code}" -b "$ROOT/bkjar" "${J[@]}" -X POST \
      -d '{"items":[{"path":"S:Startup-Sequence","dir":false}],"dir":"/out-link"}' "$A/api/copy/to-pc")
check "audit4: nor through a link in it" "403" "$out"
rm -f "$ROOT/bk-files/out-link"
out=$(stat -c %a "$ROOT/bk-cfg/wasabi/view-password" 2>/dev/null || find "$ROOT/bk-cfg" -name view-password -exec stat -c %a {} \;)
check "audit4: the password file is private" "600" "$out"
out=$(python3 - <<'PY'
import sys; sys.path.insert(0, ".")
from wasabi_api import amiga_split
print(amiga_split("Startup"), amiga_split("S:a/b"))
PY
)
check "audit4: a bare Amiga name splits sanely" "('', 'Startup') ('S:a', 'b')" "$out"

echo "test $$" > .deployed
for i in $(seq 1 25); do kill -0 $VIEW_PID 2>/dev/null || break; sleep 1; done
kill -0 $VIEW_PID 2>/dev/null; check "update: a new version stamp ends the server (Docker restarts it)" "1" "$?"
rm -f .deployed
kill $VIEW_PID 2>/dev/null

# --- error paths ---
out=$($W get L:nosuchfile /dev/null 2>&1 | grep -ci "error\|no such")
if [ "$out" -ge 1 ]; then ok "a missing file reports an error"
else no "a missing file reports an error"; fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
