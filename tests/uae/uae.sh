#!/usr/bin/env bash
# uae.sh start|stop|put|reset - a private FS-UAE for testing wasabid itself,
# on the shared clean test Amigas (~/UAE/TestMachines, `amiga-test`): its
# own copy (instance "wasabi"), its wasabid on port 1241 (WASABI_PORT=1241),
# so the shared A1200 and other sessions' emulators are never touched.
#   CONF=      A1200, Workbench 3.2.1 + MUI 5, as a 68030/50 (the default)
#   CONF=stock the same as the stock 68EC020 with 8 MB fast RAM
#   CONF=31    A1200, Workbench 3.1      CONF=mui38  3.1 + MUI 3.8
#   CONF=a600  A600, Workbench 2.05 (68000, 2 MB chip RAM)
# start (and put, while stopped) puts this tree's wasabid (BIN=wasabid.020
# for the 68020 build) in as C:wasabid; a running emulator needs
# 'wasabi update' instead. reset: a fresh copy next start. Headless, in a
# systemd scope of its own (2 GB at most); stop ends that one only.
D=$(cd "$(dirname "$0")/../.." && pwd)
P=${UAE_PORT:-1241}
export WASABI_HOST=127.0.0.1 WASABI_PORT=$P
case "$CONF" in
  ""|030) M=a1200-32-mui5; X=(--cpu 030) ;;
  stock)  M=a1200-32-mui5; X=() ;;
  31)     M=a1200-31; X=() ;;
  mui38)  M=a1200-31-mui38; X=() ;;
  a600)   M=a600-205; X=() ;;
  *) echo "uae.sh: CONF is '', stock, 31, mui38 or a600" >&2; exit 2 ;;
esac
A=(--as wasabi --port "$P")
case "$1" in
start) amiga-test $M start "${A[@]}" "${X[@]}" --wasabid "$D/${BIN:-wasabid}" && echo up ;;
put)   cp "$D/${BIN:-wasabid}" "$(amiga-test $M dir "${A[@]}")/C/wasabid" ;;
stop)  amiga-test $M stop "${A[@]}" ;;
reset) amiga-test $M reset "${A[@]}" ;;
*) echo "usage: uae.sh start|stop|put|reset" >&2; exit 2 ;;
esac
