/* doorknob.c - Dump as a doorknob SMTP forwarder
 * Copyright (C) 2018 Sean MacLennan <seanm@seanm.ca>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this project; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <syslog.h>
#include <sys/inotify.h>

static char *smtp_server;
static char *smtp_user;
static char *smtp_passwd;
static char *mail_from;
static char hostname[HOST_NAME_MAX + 1];
static int starttls;
static int rewrite_from;

static int foreground;
static long debug;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (debug)
		vprintf(fmt, ap);
	else
		vsyslog(LOG_INFO, fmt, ap);
	va_end(ap);
}

static char *must_strdup(const char *str)
{
	char *new = strdup(str);
	if (!new) {
		fputs("Out of memory!\n", stderr);
		exit(1);
	}
	return new;
}

//#define USE_CURL
#ifdef USE_CURL
#include <curl/curl.h>

// SAM fixme
static void logsmtp(const char *fname, struct curl_slist *to, CURLcode res)
{
	char log[1024];

	int n = snprintf(log, sizeof(log), "%s", fname);

	while (to) {
		n += snprintf(log + n, sizeof(log) - n, " %s", to->data);
		if (n >= sizeof(log)) goto out;
		to = to->next;
	}

	if (res == 0)
		snprintf(log + n, sizeof(log) - n, " OK");
	else
		snprintf(log + n, sizeof(log) - n,
				 " %d: %s", res, curl_easy_strerror(res));

out:
	if (foreground)
		puts(log);
	else
		syslog(LOG_INFO, "%s", log);
}

static int looking_for_from;
static size_t read_callback(char *buffer, size_t size, size_t nitems, void *fp)
{
	if (looking_for_from) {
		if (fgets(buffer, size * nitems, fp)) {
			if (strncmp(buffer, "From:", 5) == 0) {
				looking_for_from = 0;
				char *p = strchr(buffer, '<');
				if (p)
					sprintf(p + 1, "%s>\n", mail_from);
				else
					sprintf(buffer, "From: %s\n", mail_from);
			} else if (*buffer == '\n' || *buffer == '\r')
				// end of header - no From
				looking_for_from = 0;
			return strlen(buffer);
		}
		return 0;
	}

	return fread(buffer, size, nitems, fp);
}

static int smtp_one(const char *fname)
{
	FILE *fp = fopen(fname, "r");
	if (!fp) {
		perror(fname);
		return 0; // Try again
	}

	CURLcode res = 0;
	struct curl_slist *recipients = NULL;
	CURL *curl = curl_easy_init();
	if(!curl) {
		logmsg("Unable to initialize curl\n");
		goto done;
	}

	char line[128], *p;
	int first_time = 1;
	while (fgets(line, sizeof(line), fp) && *line != '\n') {
		strtok(line, "\r\n");
		if ((p = strchr(line, '@')))
			recipients = curl_slist_append(recipients, line);
		else {
			if (first_time) {
				first_time = 0;
				recipients = curl_slist_append(recipients, mail_from);
			}
		}
	}
	if (recipients == NULL) {
		logmsg("Hmmm... no to...\n");
		goto done;
	}

	curl_easy_setopt(curl, CURLOPT_URL, smtp_server);
	if (starttls)
		curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
	if (smtp_user) {
		curl_easy_setopt(curl, CURLOPT_USERNAME, smtp_user);
		curl_easy_setopt(curl, CURLOPT_PASSWORD, smtp_passwd);
	}
	curl_easy_setopt(curl, CURLOPT_MAIL_FROM, mail_from);
	curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
	curl_easy_setopt(curl, CURLOPT_READDATA, fp);
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_VERBOSE, debug);

	looking_for_from = rewrite_from;

	/* Send the message */
	res = curl_easy_perform(curl);
	logsmtp(fname, recipients, res);

done:
	fclose(fp);
	curl_slist_free_all(recipients);
	curl_easy_cleanup(curl);

	return res;
}
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

// SAM FIXME logging

// SAM FIXME move base64 into program?
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
static int base64_encode(char *dst, int dlen, const uint8_t *src, int len)
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

static int build_auth(char *buffer, int len)
{   /* smtp_user \0 smtp_user \0 smtp_passwd */
	char encode[1024];
	int len1 = strlen(smtp_user) + 1;
	int n = strlen(smtp_passwd) + len1 + len1;

	memcpy(encode, smtp_user, len1);
	memcpy(encode + len1, smtp_user, len1);
	strcpy(encode + len1 + len1, smtp_passwd);

	if (base64_encode(buffer, len - 2, (uint8_t *)encode, n) <= 0) {
		puts("base64_encode failed");
		return -1;
	}
	strcat(buffer, "\r\n"); // we left room
	return 0;
}

static int expect_status(int sock, int status)
{
	char reply[1501];
	int n = read(sock, reply, sizeof(reply) - 1);
	if (n <= 0) {
		perror("read");
		return -1;
	}
	reply[n] = 0;

	if (debug) printf("S: %s", reply);

	int got = strtol(reply, NULL, 10);
	if (status != got) {
		printf("Expected %d got %s", status, reply);
		return 1;
	}

	return 0;
}

/* Returns 0 on success, -1 on I/O error, and 1 if status is wrong */
static int send_str(int sock, const char *str, int status)
{
	if (debug) printf("C: %s", str);

	int len = strlen(str);
	int n = write(sock, str, len);
	if (n != len) {
		if (n < 0)
			perror("write");
		else
			printf("Short write: %d/%d\n", n, len);
		return -1;
	}

	return expect_status(sock, status);
}

static int send_body(int sock, FILE *fp)
{
	char buffer[4096];
	int n;

	while ((n = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
		int wrote = write(sock, buffer, n);
		if (wrote != n)
			return -1;
	}

	if (ferror(fp)) {
		perror("read file");
		return -1;
	}

	return send_str(sock, "\r\n.\r\n", 250);
}

static int smtp_one(const char *fname)
{
	char buffer[1024];
	short port = 25;
	int rc = -1;

	char *p = strstr(smtp_server, "://");
	if (p) {
		*p = 0;
		p += 3;
// SAM		if (strcmp(smtp_server, "smtps") == 0)
// SAM			port = 465;
	} else
		p = smtp_server;

	struct hostent *host = gethostbyname(p);
	if (!host) {
		printf("Unable to get host %s\n", p);
		return -1;
	}

	FILE *fp = fopen(fname, "r");
	if (!fp) {
		perror(fname);
		return -1;
	}

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		perror("socket");
		goto done;
	}

	int flags = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));

	struct sockaddr_in sock_name;
	memset(&sock_name, 0, sizeof(sock_name));
	sock_name.sin_family = AF_INET;
	sock_name.sin_addr.s_addr = *(unsigned *)host->h_addr_list[0];
	sock_name.sin_port = htons(port);

	if (connect(sock, (struct sockaddr *)&sock_name, sizeof(sock_name))) {
		perror("connect");
		goto done;
	}

	expect_status(sock, 220);

	snprintf(buffer, sizeof(buffer), "EHLO %s\r\n", hostname);
	if (send_str(sock, buffer, 250))
		goto done;

	if (smtp_user && smtp_passwd) {
#if 0
		/* This is probably more correct */
		if (send_str(sock, "AUTH PLAIN\r\n", 334))
			goto done;

		if (build_auth(buffer, sizeof(buffer))) {
			puts("base64_encode failed");
			goto done;
		}
#else
		/* This saves a message and reply */
		strcpy(buffer, "AUTH PLAIN ");
		if (build_auth(buffer + 11, sizeof(buffer) - 11)) {
			puts("base64_encode failed");
			goto done;
		}
#endif

		if (send_str(sock, buffer, 235))
			goto done;
	}

	snprintf(buffer, sizeof(buffer), "MAIL FROM:<%s>\r\n", mail_from);
	if (send_str(sock, buffer, 250))
		goto done;

	char line[128];
	int first_time = 1;
	int count = 0;
	while (fgets(line, sizeof(line), fp) && *line != '\n') {
		strtok(line, "\r\n");
		if ((p = strchr(line, '@'))) {
			snprintf(buffer, sizeof(buffer), "RCPT TO:<%s>\r\n", line);
		} else {
			if (first_time) {
				first_time = 0;
				snprintf(buffer, sizeof(buffer), "RCPT TO:<%s>\r\n", mail_from);
			} else
				continue;
		}
		int n = send_str(sock, buffer, 250);
		if (n == 0)
			++count;
		else if (n < 0)
			goto done;
		// else	ok to fail
	}

	send_str(sock, "DATA\r\n", 354);

	send_body(sock, fp);

	send_str(sock, "QUIT\r\n", 221);

	rc = 0; // success

done:
	fclose(fp);
	if (sock != -1)
		close(sock);

	return rc;
}
#endif

#define NEED_VAL if (!val) {					\
		printf("%s needs a value\n", key);		\
		continue;								\
	}

static void read_config(void)
{
	FILE *fp = fopen(CONFIG_FILE, "r");
	if (!fp) {
		perror(CONFIG_FILE);
		exit(1);
	}

	char line[128];
	while (fgets(line, sizeof(line), fp)) {
		if (*line == '#') continue;
		char *key = strtok(line, " \t\r\n");
		char *val = strtok(NULL, "\r\n");
		if (!key) continue; // empty line
		if (strcmp(key, "smtp-server") == 0) {
			NEED_VAL;
			smtp_server = must_strdup(val);
		} else if (strcmp(key, "smtp-user") == 0) {
			NEED_VAL;
			smtp_user = must_strdup(val);
		} else if (strcmp(key, "smtp-password") == 0) {
			NEED_VAL;
			smtp_passwd = must_strdup(val);
		} else if (strcmp(key, "mail-from") == 0) {
			NEED_VAL;
			mail_from = must_strdup(val);
		} else if (strcmp(key, "starttls") == 0)
			starttls = 1;
		else if (strcmp(key, "rewrite-from") == 0)
			rewrite_from = 1;
		else
			printf("Unexpected key %s\n", key);
	}

	fclose(fp);

	if (!smtp_server) {
		fputs("You must set smtp-server\n", stderr);
		exit(1);
	}
	// For now let's require mail-from
	if (!mail_from) {
		fputs("You must set mail-from\n", stderr);
		exit(1);
	}
	if (smtp_user && !smtp_passwd) {
		fputs("You must set smtp-user AND smtp-password\n", stderr);
		exit(1);
	}
}

// This is really to get around the read() return warning.
// The event is just a trigger, we don't care what it says.
static int read_event(int fd)
{
	uint8_t event[sizeof(struct inotify_event) + NAME_MAX + 1];
	return read(fd, event, sizeof(event));
}

static void usage(void)
{
	puts("usage: doorknob [-FD]\n"
		 "where: -F keeps doorknob in foreground\n"
		 "       -D turns on debugging (enables foreground)");
	exit(1);
}

int main(int argc, char *argv[])
{
	int c;

	while ((c = getopt(argc, argv, "hFD")) != EOF)
		switch (c) {
		case 'h': usage();
		case 'F': foreground = 1; break;
		case 'D': debug = 1; break;
		default: puts("Sorry!"); exit(1);
		}

	if (!foreground) debug = 0;

	read_config();

	if (gethostname(hostname, sizeof(hostname))) {
		perror("hostname");
		exit(1);
	}

	if (chdir(MAILDIR)) {
		perror(MAILDIR);
		exit(1);
	}
	if (chdir("queue")) {
		printf(MAILDIR "/queue: %s\n", strerror(errno));
		exit(1);
	}

	int fd = inotify_init();
	if (fd < 0) {
		perror("inotify_init");
		exit(1);
	}

	int watch = inotify_add_watch(fd, ".", IN_CLOSE_WRITE | IN_MOVED_TO);
	if (watch < 0) {
		perror("inotify_add_watch");
		exit(1);
	}

	if (foreground == 0) {
		if (daemon(1, 0))
			perror("daemon");
		openlog("doorknob", 0, LOG_MAIL);
	}

	struct pollfd ufd = { .fd = fd, .events = POLLIN };

	while (1) {
		DIR *dir = opendir(".");
		if (!dir) {
			logmsg("opendir");
			continue;
		}

		struct dirent *ent;
		while ((ent = readdir(dir)))
			if (*ent->d_name != '.')
				if (smtp_one(ent->d_name) == 0)
					unlink(ent->d_name);

		closedir(dir);

		// Timeout every hour
		if (poll(&ufd, 1, 3600000) == 1)
			read_event(fd);
	}

	return 0;
}
