#!/usr/bin/env bash
# uae.sh start|stop|put - a private FS-UAE for testing wasabid itself: the
# 68030/50 A1200 with OS 3.2 and the emulated bsdsocket, on a copy of the
# drive in /tmp/wasabi-uae, its wasabid on port 1241 (WASABI_PORT=1241),
# so the shared A1200 and other sessions' emulators (1234, 1240) are never
# touched. put: this tree's wasabid as C:wasabid (start does it too; a
# running emulator needs 'wasabi update' instead). Headless, in a systemd
# scope of its own (2 GB at most); stop ends that scope only.
D=$(cd "$(dirname "$0")/../.." && pwd)
W=/tmp/wasabi-uae; H="$W/AmigaOS3.2"; U=wasabi-uae
# UAE_PORT moves it off 1241 when another session's emulator has it.
P=${UAE_PORT:-1241}
export WASABI_HOST=127.0.0.1 WASABI_PORT=$P
# BIN=wasabid.020 puts the 68020 build in instead. CONF=stock runs a
# stock A1200 (68EC020 at 14 MHz, 8 MB fast RAM) instead of the 030/50.
# CONF=a600 runs the stock A600 with Workbench 2.05 (68000, 2 MB chip
# RAM, ECS) from a copy of its drive in /tmp/wasabi-uae/A600-WB2.05.
[ "$CONF" = a600 ] && H="$W/A600-WB2.05"
put() {
  cp "$D/${BIN:-wasabid}" "$H/C/wasabid"
  sed -i -E "s#(Run >NIL: C:wasabid) [0-9]+#\1 $P#" "$H/S/User-Startup"
}
case "$1" in
start)
  if systemctl --user is-active --quiet $U.scope; then
    echo "uae.sh: ours runs already"; exit 0
  fi
  if ss -ltn 2>/dev/null | grep -q ":$P "; then
    echo "uae.sh: port $P is taken (another emulator?) - try UAE_PORT=1245" >&2
    exit 1
  fi
  mkdir -p "$W"
  if [ "$CONF" = a600 ] && [ ! -d "$H" ]; then
    cp -a "$HOME/FS-UAE/Hard Drives/A600-WB2.05" "$H"
    key=$(cat "$W/AmigaOS3.2/Prefs/Env-Archive/wasabi.key" 2>/dev/null)
    [ -n "$key" ] || { echo "uae.sh: start the A1200 copy once first (its key)" >&2; exit 1; }
    printf '%s' "$key" > "$H/Prefs/Env-Archive/wasabi.key"
    printf 'Stack 65536\nRun >NIL: C:wasabid 1241\n' >> "$H/S/User-Startup"
  elif [ ! -d "$H" ]; then
    cp -a "$HOME/FS-UAE/Hard Drives/AmigaOS3.2-030" "$H"
    sed -i -e 's#^  Run >NIL: C:wasabid$#  Run >NIL: C:wasabid 1241#' "$H/S/User-Startup"
    grep -q "wasabid 1241" "$H/S/User-Startup" || { echo "uae.sh: User-Startup not patched" >&2; exit 1; }
  fi
  base="$HOME/FS-UAE/Configurations/A1200-030-50.fs-uae"
  [ "$CONF" = stock ] && base="$HOME/Projects/CTerm/tests/stock/A1200-Stock-fast.fs-uae"
  [ "$CONF" = a600 ] && base="$HOME/FS-UAE/Configurations/A600-WB2.05.fs-uae"
  sed -e "s#^hard_drive_0 = .*#hard_drive_0 = $H#" -e '/^hard_drive_[12]/d' \
      -e '/^bsdsocket_library/d' "$base" > "$W/wasabi.fs-uae"
  echo "bsdsocket_library = 1" >> "$W/wasabi.fs-uae"
  put
  (cd ~/FS-UAE && SDL_VIDEODRIVER=offscreen systemd-run --user --scope --unit=$U \
     -p MemoryMax=2G fs-uae "$W/wasabi.fs-uae" >"$W/uae.out" 2>&1 &)
  for i in $(seq 1 60); do "$D/wasabi" ping >/dev/null 2>&1 && break; sleep 2; done
  "$D/wasabi" ping >/dev/null 2>&1 || { echo "uae.sh: no wasabi on $P" >&2; exit 1; }
  echo "up"
  ;;
put) put ;;
stop)
  systemctl --user stop $U.scope 2>/dev/null
  while systemctl --user is-active --quiet $U.scope; do sleep 1; done
  ;;
*) echo "usage: uae.sh start|stop|put" >&2; exit 2 ;;
esac
