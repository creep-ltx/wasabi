/*
 * tls - Wasabi's encryption: TLS 1.3 on AmiSSL 5, when AmiSSL and keys are here.
 *
 * Both ends prove who they are with a certificate signed by the user's
 * own Wasabi CA (made on the PC by `wasabi keys init`): this end shows
 * ENVARC:wasabi/amiga.crt, the client must show one signed by
 * ENVARC:wasabi/ca.crt. Dates are not checked - the Amiga's clock may be
 * wrong; the signature is what is trusted. After the handshake the
 * daemon's own key check (HELLO) still applies, inside the tunnel.
 *
 * Kept in its own file because AmiSSL's headers and the daemon's do not
 * mix, and because this is the one place that knows what an SSL is.
 */
#ifndef WASABI_TLS_H
#define WASABI_TLS_H

#include <exec/types.h>

#define TLS_DIR "ENVARC:wasabi/"

/* Open AmiSSL and load the keys. NULL when ready; otherwise what is
 * wrong, in words for the person. Safe to call again after a failure. */
const char *tls_open(void);
void tls_close(void);
BOOL tls_ready(void);

/* A freshly accepted socket: non-blocking, server side, handshake not
 * yet run. NULL if that cannot even begin. */
void *tls_start(LONG fd);

/* Carry the handshake on: 1 done, 0 wants to read, 2 wants to write,
 * -1 failed (the caller drops the client). */
int tls_step(void *c);

/* > 0 bytes read; 0 wants to read; -2 wants to write; -1 closed/failed. */
LONG tls_read(void *c, UBYTE *buf, LONG len);

/* > 0 bytes written (all of len: no partial writes); 0 wants to write;
 * -2 wants to read; -1 failed. On 0 or -2 call again with the SAME
 * buffer and length once the socket is ready. */
LONG tls_write(void *c, const UBYTE *buf, LONG len);

/* Decrypted bytes already buffered: the socket will not say "readable"
 * for them, so the main loop must ask. */
LONG tls_pending(void *c);

/* close_notify (once, never waits), then free. */
void tls_bye(void *c);
void tls_free(void *c);

#endif
