#include "doorknob.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

char *must_strdup(const char *str)
{
	char *new = strdup(str);
	if (!new) {
		fputs("Out of memory!\n", stderr);
		exit(1);
	}
	return new;
}

#ifdef __linux__
size_t strlcpy(char *dst, const char *src, size_t dstsize)
{
	int srclen = strlen(src);

	if (dstsize > 0) {
		if (dstsize > srclen)
			strcpy(dst, src);
		else {
			strncpy(dst, src, dstsize - 1);
			dst[dstsize - 1] = 0;
		}
	}

	return srclen;
}

size_t strlcat(char *dst, const char *src, size_t dstsize)
{
	int dstlen = strlen(dst);
	int srclen = strlen(src);
	int left   = dstsize - dstlen;

	if (left > 0) {
		if (left > srclen)
			strcpy(dst + dstlen, src);
		else {
			strncpy(dst + dstlen, src, left - 1);
			dst[dstsize - 1] = 0;
		}
	}

	return dstlen + srclen;
}
#endif

/* Concatenates any number of strings. The last string must be NULL. */
void strconcat(char *str, int len, ...)
{
	char *arg;

	va_list ap;
	va_start(ap, len);
	while ((arg = va_arg(ap, char *)) && len > 0) {
		int n = strlcpy(str, arg, len);
		str += n;
		len -= n;
	}
	va_end(ap);
}
