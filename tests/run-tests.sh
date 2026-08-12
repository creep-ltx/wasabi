#!/usr/bin/env bash
# Exercise the wasabi client against the host mock daemon. No Amiga
# required - prove the client on the host before trusting it on iron.
set -u

cd "$(dirname "$0")/.."

PORT=${PORT:-14231}
KEY=hunter2
ROOT=$(mktemp -d /tmp/wasabi-test.XXXXXX)
CACHE=$HOME/.cache/wasabi/last-host
PASS=0
FAIL=0

cleanup() {
    [ -n "${MOCK_PID:-}" ] && kill "$MOCK_PID" 2>/dev/null
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

# --- handshake ---
out=$($W ping 2>&1 | grep -c "mock-wasabid")
check "ping completes the handshake" "1" "$out"

out=$(./wasabi --host 127.0.0.1 --port "$PORT" --key wrong ping 2>&1 | \
      grep -c "bad key")
check "a wrong key is refused" "1" "$out"

# --- listing ---
out=$($W ls C: 2>/dev/null | grep -c "greet.txt")
check "ls shows a file" "1" "$out"

# --- round trip, multi-frame ---
head -c 200000 /dev/urandom > "$ROOT/../wasabi-big.$$"
$W put "$ROOT/../wasabi-big.$$" L:big >/dev/null 2>&1
$W get L:big "$ROOT/../wasabi-back.$$" >/dev/null 2>&1
if cmp -s "$ROOT/../wasabi-big.$$" "$ROOT/../wasabi-back.$$"; then
    ok "200000-byte put/get round trip is byte-identical"
else
    no "200000-byte put/get round trip is byte-identical"
fi
rm -f "$ROOT/../wasabi-big.$$" "$ROOT/../wasabi-back.$$"

# --- put is atomic: no temp file left behind ---
out=$(find "$ROOT" -name "*.wasabi-tmp" | wc -l)
check "put leaves no temp file behind" "0" "$out"

# --- run: output and exit code ---
out=$($W run "echo one; echo two" 2>/dev/null | tr '\n' ',')
check "run streams stdout in order" "one,two," "$out"

$W run "exit 7" >/dev/null 2>&1
check "run propagates the exit code" "7" "$?"

out=$($W run "echo to-stderr >&2" 2>&1 >/dev/null)
check "run keeps stderr separate" "to-stderr" "$out"

# --- mkdir / del ---
$W mkdir L:newdrawer >/dev/null 2>&1
check "mkdir creates a drawer" "0" "$?"
[ -d "$ROOT/L/newdrawer" ] && ok "the drawer really exists" \
                           || no "the drawer really exists"

$W del L:big >/dev/null 2>&1
[ ! -f "$ROOT/L/big" ] && ok "del removes a file" || no "del removes a file"

# --- streams ---
out=$(timeout -s INT 1.5 $W snoop --task cfile 2>/dev/null | \
      grep -c "^cfile")
if [ "$out" -ge 2 ]; then ok "snoop honours the task filter"
else no "snoop honours the task filter (got $out lines)"; fi

out=$(timeout -s INT 1.5 $W snoop --task cfile 2>/dev/null | grep -c "Shell")
check "snoop filter suppresses other tasks" "0" "$out"

out=$(timeout -s INT 1.2 $W debug 2>/dev/null | grep -c "ReadCacheNode")
if [ "$out" -ge 1 ]; then ok "debug streams lines"
else no "debug streams lines"; fi

# --- combined view and --log ---
STREAMLOG=$ROOT/../wasabi-streamlog.$$
out=$(timeout -s INT 1.6 $W debug --with-snoop --log "$STREAMLOG" 2>/dev/null)
d=$(printf '%s\n' "$out" | grep -c "^debug | ")
s=$(printf '%s\n' "$out" | grep -c "^snoop | ")
if [ "$d" -ge 1 ] && [ "$s" -ge 1 ]; then ok "combined view carries both streams"
else no "combined view carries both streams (debug $d, snoop $s)"; fi

n=$(grep -cE \
    '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} (debug|snoop) \| ' \
    "$STREAMLOG" 2>/dev/null)
if [ "${n:-0}" -ge 2 ]; then ok "--log stamps every line"
else no "--log stamps every line (got ${n:-0})"; fi
rm -f "$STREAMLOG"

# --- deploy --restart: upload then reload the daemon in one shot ---
echo restarter > "$ROOT/../wasabi-restart.$$"
out=$($W deploy "$ROOT/../wasabi-restart.$$" C:wasabid.new --restart 2>&1 | \
      grep -c "reloading itself")
check "deploy --restart uploads and reloads" "1" "$out"
rm -f "$ROOT/../wasabi-restart.$$"

# --- update: the daemon may only be replaced through verification ---
NEWD=$ROOT/../wasabi-newd.$$
printf 'binary\0$VER: wasabid 9.9test (1.1.2026)\0rest\n' > "$NEWD"
printf 'old daemon\n' > "$ROOT/C/wasabid"

out=$($W put "$NEWD" C:wasabid 2>&1 | grep -c "wasabi update")
check "put refuses to overwrite the running daemon" "1" "$out"
check "and leaves it alone" "old daemon" "$(cat "$ROOT/C/wasabid")"

out=$($W update /etc/hostname 2>&1 | grep -c "not a wasabid binary")
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
if [ -s "$SHOT" ] && head -c8 "$SHOT" | grep -q PNG; then
    ok "grab writes a real PNG"
else
    no "grab writes a real PNG"
fi
out=$(python3 -c "
import struct,sys
d=open('$SHOT','rb').read()
print('%dx%d' % struct.unpack('>II', d[16:24]))" 2>/dev/null)
check "and its dimensions match what was sent" "8x4" "$out"
rm -f "$SHOT"

out=$($W screen 2>/dev/null | grep -c "CygnusEd Professional V4.2")
check "screen lists what is open" "1" "$out"
out=$($W screen 2>/dev/null | grep -c "<- front")
check "and marks the front one" "1" "$out"

# --- ps / kill ---
out=$($W ps 2>/dev/null | grep -c "input.device")
check "ps lists tasks" "1" "$out"

out=$($W ps 'input#?' 2>/dev/null | grep -c "device")
check "ps honours a filter" "1" "$out"

$W kill Wait >/dev/null 2>&1
check "kill by command name succeeds" "0" "$?"

out=$($W kill nosuchtask 2>&1 | grep -c "no task")
check "kill of a missing task errors" "1" "$out"

out=$($W kill con_handler 2>&1 | grep -c "ambiguous")
check "kill of an ambiguous name errors" "1" "$out"

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
rm -f "$HOME/.cache/wasabi/refused-127.0.0.1"
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
rm -f "$HOME/.cache/wasabi/refused-127.0.0.1"

# --- error paths ---
out=$($W get L:nosuchfile /dev/null 2>&1 | grep -ci "error\|no such")
if [ "$out" -ge 1 ]; then ok "a missing file reports an error"
else no "a missing file reports an error"; fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
