#!/usr/bin/env bash
# fetch.sh - download the AmiSSL 5.27 SDK (headers, link stubs) that the
# Amiga program's TLS builds on. Not kept in git. The archive must be the
# one this was written against: its SHA-256 is checked before unpacking.
set -e
cd "$(dirname "$0")"
SHA256=5003bef8c5930354d16b0ce7196d71b2811891c42fad38a9238c5ce4098ad42a
URL=https://github.com/jens-maus/amissl/releases/download/5.27/AmiSSL-5.27-SDK.lha
if [ ! -d AmiSSL/Developer ]; then
  [ -f AmiSSL-5.27-SDK.lha ] || curl -fsSL -o AmiSSL-5.27-SDK.lha "$URL"
  echo "$SHA256  AmiSSL-5.27-SDK.lha" | sha256sum -c --quiet || {
    echo "AmiSSL-5.27-SDK.lha is not the expected file; deleted" >&2
    rm -f AmiSSL-5.27-SDK.lha; exit 1; }
  lha xq AmiSSL-5.27-SDK.lha
fi
echo "third_party ready"
