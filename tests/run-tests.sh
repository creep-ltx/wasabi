#!/usr/bin/env bash
# Exercise the wasabi client against the host mock daemon. No Amiga
# required - this is the harness-first road the rest of the suite uses.
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

out=$(timeout -s INT 1.2 $W debug 2>/dev/null | grep -c "ReadCacheNode")
if [ "$out" -ge 1 ]; then ok "debug streams lines"
else no "debug streams lines"; fi

# --- error paths ---
out=$($W get L:nosuchfile /dev/null 2>&1 | grep -ci "error\|no such")
if [ "$out" -ge 1 ]; then ok "a missing file reports an error"
else no "a missing file reports an error"; fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
