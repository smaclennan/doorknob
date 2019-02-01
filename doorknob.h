#include <stdint.h>
#include <string.h>

/* Exported from doorknob.c */
void logmsg(const char *fmt, ...);

/* Exported from openssl.c */
int openssl_open(int sock, const char *host);
int openssl_read(char *buffer, int len);
int openssl_write(const char *buffer, int len);
void openssl_close(void);

/* Exported from base64.c */
int base64_encode(char *dst, int dlen, const uint8_t *src, int len);
int mkauthplain(const char *user, const char *passwd, char *plain, int len);

/* Exported from utils.c */
char *must_strdup(const char *str);
void strconcat(char *str, int len, ...);
#ifdef __linux__
size_t strlcpy(char *dst, const char *src, size_t dstsize);
size_t strlcat(char *dst, const char *src, size_t dstsize);
#endif
