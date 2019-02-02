#include "doorknob.h"

static char alphabet[] = {
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789"
	"-_"
};

/* encode 3 bytes into 4 bytes
 *  < 6 | 2 > < 4 | 4 > < 2 | 6 >
 */
static void encode_block(char *dst, const uint8_t *src)
{
	*dst++ = alphabet[src[0] >> 2];
	*dst++ = alphabet[((src[0] << 4) & 0x30) | (src[1] >> 4)];
	*dst++ = alphabet[((src[1] << 2) & 0x3c) | (src[2] >> 6)];
	*dst++ = alphabet[src[2] & 0x3f];
}

/* dst is returned null terminated but the return does not include the null. */
int base64_encode(char *dst, int dlen, const uint8_t *src, int len)
{
	int cnt = 0;

	while (len >= 3) {
		encode_block(dst, src);
		dst += 4;
		src += 3;
		len -= 3;
		cnt += 4;
	}
	if (len > 0) {
		uint8_t block[3];
		memset(block, 0, 3);
		memcpy(block, src, len);
		encode_block(dst, block);
		dst[3] = '=';
		if (len == 1) dst[2] = '=';
		dst += 4;
		cnt += 4;
	}

	*dst = '\0';
	return cnt;
}

int mkauthplain(const char *user, const char *passwd, char *plain, int len)
{   /* user \0 user \0 passwd */
	char encode[1024];
	int len1 = strlen(user) + 1;
	int n = strlen(passwd) + len1 + len1;

	memcpy(encode, user, len1);
	memcpy(encode + len1, user, len1);
	strcpy(encode + len1 + len1, passwd);

	if (base64_encode(plain, len, (uint8_t *)encode, n) <= 0)
		return 1;

	return 0;
}
