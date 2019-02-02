#include <stdint.h>
#include <string.h>

/* Exported from doorknob.c */
void logmsg(const char *fmt, ...);

/* Exported from bear.c */
int ssl_open(int sock, const char *host);
int ssl_read(char *buffer, int len);
int ssl_write(const char *buffer, int len);
void ssl_close(void);

/* Exported from utils.c */
int base64_encode(char *dst, int dlen, const uint8_t *src, int len);
int mkauthplain(const char *user, const char *passwd, char *plain, int len);
char *must_strdup(const char *str);
void strconcat(char *str, int len, ...);
#ifdef __linux__
size_t strlcpy(char *dst, const char *src, size_t dstsize);
size_t strlcat(char *dst, const char *src, size_t dstsize);
#endif
