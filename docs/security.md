# Security

[Back to the README](../README.md)

## Security

`wasabid` executes arbitrary commands as whatever started it, on a
machine with no memory protection. **Encryption is optional (0.8+)**:
an Amiga with AmiSSL 5 and its keys speaks TLS 1.3, both ends proving
who they are, and then accepts nothing else; an Amiga without AmiSSL
works plain. The key in `ENV:wasabi.key` is checked either way.

### Encryption (wasabid 0.6+, optional since 0.8)

Your own small certificate authority, made once on your PC, signs a
certificate for each Amiga and one for the PC. Each end accepts only
certificates your CA signed: someone else's Amiga, or someone else's
computer, is refused before a single command is read, and nothing on
the wire can be read or changed.

```
wasabi keys init                    once: the CA and this computer's
                                    certificate, in ~/.config/wasabi/keys
wasabi keys amiga NAME              install an Amiga's keys (it needs
                                    AmiSSL 5; then restart wasabid)
wasabi keys drive DIR NAME          into an emulator drive's folder
wasabi keys show                    what there is, with fingerprints,
                                    and the Amigas reached encrypted
wasabi keys forget HOST             let one of those be plain again
```

The Amiga keeps its half in `ENVARC:wasabi/` (`ca.crt`, `amiga.crt`,
`amiga.key`) and needs **AmiSSL 5**. Another computer (the NAS, a
laptop) needs `ca.crt`, `client.crt` and `client.key` from the keys
folder — not `ca.key`, which stays on the computer that signs new
Amigas. Keep that folder: `wasabi keys init` refuses to replace it,
because a new CA would lock out every Amiga that has the old one.

- **Who decides.** The client encrypts whenever it has keys. An Amiga
  with TLS ready refuses plain clients (saying why) unless wasabid was
  started with `plain`. An Amiga without AmiSSL or keys takes plain
  clients, and says so at startup and in its discovery answer
  (`- unencrypted: <reason>`).
- **No silent downgrade.** A client with keys that finds an Amiga
  without encryption goes plain and prints a warning - but only to an
  Amiga it has never reached encrypted. Once it has (remembered in
  `~/.config/wasabi/tls-hosts`, like SSH's known hosts), plain to that
  address is refused: its AmiSSL or keys are gone, or something is
  answering in its place to catch the key. `wasabi keys forget HOST`
  when it was on purpose. `--plain` asks for no encryption for one
  command. An update is proven the way it runs: the test copy of the
  new daemon must accept the same kind of connection first.
- **Except on this computer itself.** A connection from `127.x` - an
  FS-UAE emulator, whose sockets are the PC's, or an SSH tunnel - may
  skip TLS: those bytes never cross a network, and anything able to
  watch them could read the keys anyway. The daemon tells the two apart
  by the first byte (a TLS ClientHello starts `0x16`). Emulators
  therefore need neither AmiSSL nor keys. `WASABI_TLS=always` encrypts
  there too.
- **Dates are not checked on the Amiga** (its clock may be wrong); the
  certificates are valid for 100 years so the PC does not trip either.
- **If TLS cannot start** (AmiSSL missing, its assign not yet made at
  boot, no keys), wasabid runs plain, says why in its discovery answer
  (`wasabi discover`), and tries again every 30 s; a client that knew
  it encrypted refuses it meanwhile, as above.
- **Cost:** Ed25519 certificates, X25519 key exchange and ChaCha20 (no
  AES instructions on a 68k), chosen because every public-key step is
  plain 68k arithmetic. Measured on an emulated 68030/50: a handshake
  takes ~12 s (P-256: 22 s) and data moves at ~29 KB/s (AES-256: 13).
  On the A1200 (Emu68): handshake 0.07 s, 11.7 MB/s up and 13 MB/s
  down, a 1280x960 grab in 0.3 s (plain was 42 MB/s into RAM:).

#### An Amiga without AmiSSL

Nothing to set up: an Amiga without AmiSSL (or without keys) answers
plain, and a client with keys prints one warning and goes plain.

The one case that stops you is an address that **was** reached
encrypted before - you removed AmiSSL or its keys, or another Amiga or
emulator now answers on the same address and port. The client refuses
plain there, because that is exactly what something pretending to be
your Amiga would look like, and the error says so. To make it plain
again:

```
wasabi keys forget 192.168.1.20     that address, every port, from now on
wasabi --plain ping                 just this one command
```

The remembered addresses are `~/.config/wasabi/tls-hosts`, one
`ADDRESS:PORT` per line; deleting a line is the same as `keys forget`.
From Python: `wasabi_client.connect(..., tls=False)` (see
[the Python API](api.md)).

**It answers only addresses that cannot be routed in from the
internet** — RFC1918 (`10/8`, `172.16/12`, `192.168/16`) plus loopback
and link-local. Anything else is closed before it can send `HELLO`. This
is not about hostile neighbours; it is about the mistake that actually
hurts a tool like this: somebody forwards port 1234, or their router
does it for them via UPnP, or the Amiga lands in a DMZ, and an
unauthenticated remote shell is now on the open internet. The check
lives in the daemon, where a router configuration cannot undo it.

Not narrower than RFC1918 on purpose. Restricting to `192.168` would
lock out every home whose ISP router hands out `10.0.0.x` — Xfinity's
default among others — and buy nothing, since `10/8` is exactly as
unroutable from outside as `192.168/16`.

Loopback is allowed deliberately: a connection arriving through an SSH
tunnel that terminates on the Amiga comes from `127.0.0.1`, so tunnels
and this check compose rather than fight.

**A VPN subnet router keeps working, and you should know why.** Tested
on this network: a laptop on a phone hotspot, reaching the Amiga over
Tailscale via a NAS advertising the LAN as a subnet route, connected
with nothing configured and nothing refused. Tailscale masquerades
subnet-route traffic by default, so the Amiga sees the *router's* LAN
address rather than the `100.64.0.0/10` one the laptop holds — and
`192.168/16` already covers that.

The consequence is worth saying out loud: with a subnet router in the
path, **everything on your tailnet reaches wasabid as though it were on
the LAN**, and the daemon cannot tell the difference. That is not a hole
— a tailnet is authenticated and you choose who is in it — but your
tailnet membership is now part of wasabi's trust boundary. Worth
remembering before sharing a node, rather than after. (`allow` is still
there for a VPN that does *not* masquerade, where the Amiga would see
the real `100.x` source.)

Which of those is happening is observable rather than guesswork:
`wasabi debug` names every connection as it arrives.

A mesh VPN is a legitimate way to reach a machine and is not RFC1918
(Tailscale uses `100.64.0.0/10`), so ranges can be added:

```
Run >NIL: C:wasabid                          plain: RFC1918 + loopback
Run >NIL: C:wasabid allow 100.64.0.0/10      plus a Tailscale range
Run >NIL: C:wasabid allow any                off - warns loudly at startup
```

Refusals are counted per address rather than logged per event, because a
port scan would otherwise write a very large file about one host. The
tally lives in `L:wasabid.refused`, survives a restart, and shows up
where you will actually see it:

```
$ wasabi ping
wasabi: 673 connection(s) refused as off-LAN since you last looked
(673 in total) - 'wasabi debug' shows them as they happen
wasabid 0.2 - 3.2 ms
```

Accepted connections are announced on the `debug` stream too, so "who
is actually talking to my Amiga" is answerable by watching rather than
inferring:

```
[wasabi: 192.168.1.10 connected]
[wasabi: refused 203.0.113.44 - not on the LAN]
```

That warning appears only when the number has *risen* since you last
connected — a standing count on every command is wallpaper within a day,
whereas one that moves is a signal. `wasabi info` always shows the
total, and each refusal appears on the `debug` stream as it happens.

The discovery responder answers anyone *on the LAN*, deliberately —
needing the key to *find* the machine would defeat the point, and it
reveals no more than a port scan. It stays silent to everyone else.
