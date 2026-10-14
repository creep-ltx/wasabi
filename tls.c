/*
 * tls - Wasabi's TLS (see tls.h), on AmiSSL 5 (OpenSSL 3 as an Amiga
 * library). Taken from AmiClaude's amiga/tls.c, which proved the recipe on
 * the A1200 (Knowledge/amiga/ssh-libssh2-amissl.md), and changed from a
 * pinned PC certificate to "signed by the Wasabi CA", so a PC, the NAS
 * and any later client can be added without touching the Amiga.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <proto/bsdsocket.h>
#include <proto/amissl.h>
#include <proto/amisslmaster.h>
#include <libraries/amisslmaster.h>
#include <amissl/tags.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include "tls.h"

#ifndef FIONBIO
#define FIONBIO 0x8004667EUL            /* _IOW('f', 126, long) */
#endif

extern struct Library *SocketBase;      /* wasabid.c opens it */
struct Library *AmiSSLBase, *AmiSSLExtBase, *AmiSSLMasterBase;

static SSL_CTX *ctx;

/* A whole small file, into a memory BIO - read by dos.library, so Amiga
 * paths work. The BIO is a view of buf: free it before the next read. */
static BIO *file_bio(const char *path)
{
    static char buf[8192];
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    LONG n;
    if (!fh)
        return NULL;
    n = Read(fh, buf, sizeof buf);
    Close(fh);
    if (n <= 0 || n == sizeof buf)      /* empty, or too big to be ours */
        return NULL;
    return BIO_new_mem_buf(buf, n);
}

static X509 *load_cert(const char *path)
{
    BIO *b = file_bio(path);
    X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
    if (b)
        BIO_free(b);
    return x;
}

/* The key has no password; never ask for one (it would wait on stdin,
 * which for a daemon started by Run >NIL: is nothing, forever). */
static int no_password(char *buf, int size, int rw, void *u)
{
    return 0;
}

static EVP_PKEY *load_key(const char *path)
{
    BIO *b = file_bio(path);
    EVP_PKEY *k = b ? PEM_read_bio_PrivateKey(b, NULL, no_password, NULL)
                    : NULL;
    if (b)
        BIO_free(b);
    return k;
}

BOOL tls_ready(void)
{
    return ctx != NULL;
}

const char *tls_open(void)
{
    X509 *mine, *ca;
    EVP_PKEY *key;
    X509_STORE *store;
    BOOL ok;

    if (ctx)
        return NULL;
    if (!AmiSSLMasterBase) {
        AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
                                       AMISSLMASTER_MIN_VERSION);
        if (!AmiSSLMasterBase)
            return "no AmiSSL 5 (amisslmaster.library) - Wasabi needs it "
                   "for its encrypted connection";
        if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                           AmiSSL_UsesOpenSSLStructs, FALSE,
                           AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                           AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                           AmiSSL_SocketBase, (ULONG)SocketBase,
                           AmiSSL_ErrNoPtr, (ULONG)&errno, TAG_DONE)) {
            CloseLibrary(AmiSSLMasterBase);
            AmiSSLMasterBase = NULL;
            return "AmiSSL would not open";
        }
    }
    mine = load_cert(TLS_DIR "amiga.crt");
    key = load_key(TLS_DIR "amiga.key");
    ca = load_cert(TLS_DIR "ca.crt");
    if (!mine || !key || !ca) {
        if (mine) X509_free(mine);
        if (key) EVP_PKEY_free(key);
        if (ca) X509_free(ca);
        ERR_clear_error();
        return "no keys in ENVARC:wasabi/ - on the PC: wasabi keys amiga";
    }
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        X509_free(mine); EVP_PKEY_free(key); X509_free(ca);
        ERR_clear_error();
        return "AmiSSL could not set up TLS";
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    /* ChaCha20 first, and our choice over the client's: a 68k has no AES
     * instructions, and ChaCha20 is the cipher made for plain CPUs. */
    SSL_CTX_set_ciphersuites(ctx, "TLS_CHACHA20_POLY1305_SHA256:"
                             "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384");
    SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set1_groups_list(ctx, "X25519:P-256");
    /* A key that does not belong to its certificate (half a deploy)
     * would fail every handshake later and look like the PC's fault. */
    ok = SSL_CTX_use_certificate(ctx, mine) == 1 &&
         SSL_CTX_use_PrivateKey(ctx, key) == 1 &&
         SSL_CTX_check_private_key(ctx) == 1;
    X509_free(mine);                    /* the context keeps its own */
    EVP_PKEY_free(key);
    store = SSL_CTX_get_cert_store(ctx);
    if (ok)
        ok = X509_STORE_add_cert(store, ca) == 1;
    X509_free(ca);
    ERR_clear_error();
    if (!ok) {
        SSL_CTX_free(ctx);
        ctx = NULL;
        return "the keys in ENVARC:wasabi/ do not fit together - on the "
               "PC: wasabi keys amiga";
    }
    X509_STORE_set_flags(store, X509_V_FLAG_NO_CHECK_TIME);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER |
                       SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    /* The same buffer is always passed again after WANT_WRITE, but the
     * daemon's frames come from more than one buffer over its life. */
    SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return NULL;
}

void tls_close(void)
{
    if (ctx) {
        SSL_CTX_free(ctx);
        ctx = NULL;
    }
    if (AmiSSLMasterBase) {
        CloseAmiSSL();
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        AmiSSLBase = AmiSSLExtBase = NULL;
    }
}

void *tls_start(LONG fd)
{
    LONG one = 1;
    SSL *s;
    if (!ctx)
        return NULL;
    IoctlSocket(fd, FIONBIO, (char *)&one);
    s = SSL_new(ctx);
    if (!s)
        return NULL;
    SSL_set_fd(s, fd);
    SSL_set_accept_state(s);
    return s;
}

int tls_step(void *c)
{
    int r = SSL_do_handshake((SSL *)c), e;
    if (r == 1)
        return 1;
    e = SSL_get_error((SSL *)c, r);
    ERR_clear_error();
    if (e == SSL_ERROR_WANT_READ)
        return 0;
    if (e == SSL_ERROR_WANT_WRITE)
        return 2;
    return -1;
}

LONG tls_read(void *c, UBYTE *buf, LONG len)
{
    int r = SSL_read((SSL *)c, buf, len), e;
    if (r > 0)
        return r;
    e = SSL_get_error((SSL *)c, r);
    ERR_clear_error();
    if (e == SSL_ERROR_WANT_READ)
        return 0;
    if (e == SSL_ERROR_WANT_WRITE)
        return -2;
    return -1;
}

LONG tls_write(void *c, const UBYTE *buf, LONG len)
{
    int r = SSL_write((SSL *)c, buf, len), e;
    if (r > 0)
        return r;
    e = SSL_get_error((SSL *)c, r);
    ERR_clear_error();
    if (e == SSL_ERROR_WANT_WRITE)
        return 0;
    if (e == SSL_ERROR_WANT_READ)
        return -2;
    return -1;
}

LONG tls_pending(void *c)
{
    return c ? SSL_pending((SSL *)c) : 0;
}

void tls_bye(void *c)
{
    if (c && SSL_is_init_finished((SSL *)c)) {
        SSL_shutdown((SSL *)c);         /* once: the socket is non-blocking */
        ERR_clear_error();
    }
}

void tls_free(void *c)
{
    if (c)
        SSL_free((SSL *)c);
}
