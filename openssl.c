#ifdef WANT_OPENSSL
#include "doorknob.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

/* Application-wide SSL context. This is common to all SSL
 * connections.  */
static SSL_CTX *ssl_ctx;
static SSL *ssl;

static void print_errors(void)
{
	unsigned long err;
	while ((err = ERR_get_error()))
		printf("OpenSSL: %s\n", ERR_error_string(err, NULL));
}

static int openssl_init(void)
{
	if (ssl_ctx)
		return 0;

	if (RAND_status() != 1) {
		printf("Could not seed PRNG\n");
		return -ENOENT;
	}

	SSL_library_init();
	SSL_load_error_strings();

	ssl_ctx = SSL_CTX_new(SSLv23_client_method());
	if (!ssl_ctx) {
		print_errors();
		return 1;
	}

	/* SSL_VERIFY_NONE instructs OpenSSL not to abort SSL_connect
	 * if the certificate is invalid. */
	SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);

	SSL_CTX_set_mode(ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);

	SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);

	return 0;
}

int openssl_open(int sock, const char *host)
{
	if (openssl_init())
		return 1;

	ssl = SSL_new(ssl_ctx);
	if (!ssl)
		goto error;

	if (SSL_set_tlsext_host_name(ssl, host) == 0) // SAM fixme
		goto error;

	if (!SSL_set_fd(ssl, sock))
		goto error;

	SSL_set_connect_state(ssl);

	if (SSL_connect(ssl) <= 0)
		goto error;

	return 0;

error:
	printf("SSL handshake failed.\n");
	print_errors();
	return 1;
}

int openssl_read(char *buffer, int len)
{
	int n, err;

	do
		n = SSL_read(ssl, buffer, len);
	while (n < 0 &&
	       (err = SSL_get_error(ssl, n)) == SSL_ERROR_SYSCALL &&
	       errno == EINTR);

	if (n < 0)
		switch (err) {
		case SSL_ERROR_WANT_READ: // SAM can these happen blocking?
		case SSL_ERROR_WANT_WRITE:
			return -EAGAIN;
		default:
			printf("Not read or write read %d\n", err);
			print_errors();
			break;
		}

	return n;
}

int openssl_write(const char *buffer, int len)
{
	int n, err;

	do
		n = SSL_write(ssl, buffer, len);
	while (n < 0 &&
	       (err = SSL_get_error(ssl, n)) == SSL_ERROR_SYSCALL &&
	       errno == EINTR);

	if (n < 0)
		switch (err) {
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE:
			return -EAGAIN;
		default:
			printf("Not read or write write %d\n", err);
			print_errors();
			break;
		}

	return n;
}

void openssl_close(void)
{
	if (ssl) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
		ssl = NULL;
	}
}
#endif

