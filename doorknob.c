/* doorknob.c - Dumb as a doorknob SMTP forwarder
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
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/inotify.h>

#include "doorknob.h"

static char *smtp_server;
static char *smtp_user;
static char *smtp_passwd;
static char *mail_from;
static int starttls;
static int rewrite_from;

static int foreground;
static long debug;
static int use_stderr;

#ifdef WANT_SSL
static int use_ssl;
#endif

#ifndef WANT_CURL
static short smtp_port = 25;
static uint32_t smtp_addr; // ipv4 only
static char hostname[HOST_NAME_MAX + 1];
#endif

static inline int write_string(char *str)
{
	strcat(str, "\n");
	return write(2, str, strlen(str));
}

void logmsg(const char *fmt, ...)
{
	va_list ap;
	char msg[128];

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
	va_end(ap);

	if (use_stderr)
		write_string(msg);
	else
		syslog(LOG_INFO, "%s", msg);
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

#ifdef __linux__
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

static int open_spool_file(const char *fname, FILE **fp)
{
	struct stat sbuf;
	if (stat(fname, &sbuf))
		return -1;

	if (!S_ISREG(sbuf.st_mode)) {
		logmsg("%s: Not a regular file", fname);
		return 1; // try to delete it
	}

	*fp = fopen(fname, "r");
	if (!*fp)
		return -1;

	return 0;
}

#ifdef WANT_CURL
#include <curl/curl.h>

static void logsmtp(const char *fname, struct curl_slist *to, CURLcode res)
{
	char log[1024];
	*log = 0;

	strlcat(log, fname, sizeof(log));

	while (to) {
		strlcat(log, " ", sizeof(log));
		strlcat(log, to->data, sizeof(log));
		to = to->next;
	}

	if (res == 0)
		strlcat(log, " OK", sizeof(log));
	else {
		int n = strlen(log);
		if (sizeof(log) > n)
			snprintf(log + n, sizeof(log) - n,
					 " %d: %s", res, curl_easy_strerror(res));
	}

	logmsg("%s", log);
}

static int smtp_one(const char *fname)
{
	FILE *fp;

	int rc = open_spool_file(fname, &fp);
	if (rc)
		return rc;

	CURLcode res = 0;
	struct curl_slist *recipients = NULL;
	CURL *curl = curl_easy_init();
	if(!curl) {
		logmsg("Unable to initialize curl");
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
		logmsg("Hmmm... no to...");
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

static int read_socket(int sock, void *buf, int count)
{
#ifdef WANT_SSL
	if (use_ssl)
		return openssl_read(buf, count);
	else
#endif
		return read(sock, buf, count);
}

static int write_socket(int sock, const void *buf, int count)
{
#ifdef WANT_SSL
	if (use_ssl)
		return openssl_write(buf, count);
	else
#endif
		return write(sock, buf, count);
}

/* This is global so other functions can parse the reply */
static char reply[1501];

static int expect_status(int sock, int status)
{
	int n = read_socket(sock, reply, sizeof(reply) - 1);
	if (n <= 0) {
		logmsg("read: %s", strerror(errno));
		return -1;
	}
	reply[n] = 0;

	if (debug) printf("S: %s", reply);

	int got = strtol(reply, NULL, 10);
	if (status != got) {
		logmsg("Expected %d got %s", status, reply);
		return 1;
	}

	return 0;
}

/* Returns 0 on success, -1 on I/O error, and 1 if status is wrong */
static int send_str(int sock, const char *str, int status)
{
	if (debug) printf("C: %s", str);

	int len = strlen(str);
	int n = write_socket(sock, str, len);
	if (n != len) {
		if (n < 0)
			logmsg("write %s: %s", str, strerror(errno));
		else
			logmsg("Short write: %d/%d", n, len);
		return -1;
	}

	return expect_status(sock, status);
}

static int send_body(int sock, FILE *fp)
{
	char buffer[4096];
	int n;

	looking_for_from = rewrite_from;

	// This could be more efficient for the rewrite_from case
	while ((n = read_callback(buffer, 1, sizeof(buffer), fp)) > 0) {
		int wrote = write_socket(sock, buffer, n);
		if (wrote != n)
			return -1;
	}

	if (ferror(fp)) {
		logmsg("read file: %s", strerror(errno));
		return -1;
	}

	return send_str(sock, "\r\n.\r\n", 250);
}

#define AUTH_TYPE_PLAIN 1
#define AUTH_TYPE_LOGIN 2

static int auth_type; // set from ehlo reply

static int send_ehlo(int sock)
{
	char buffer[128], *p, *e;

	snprintf(buffer, sizeof(buffer), "EHLO %s\r\n", hostname);
	if (send_str(sock, buffer, 250))
		return -1;

	// For starttls this may change so reset
	auth_type = 0;

	if ((p = strstr(reply, "250-AUTH"))) {
		if ((e = strchr(p, '\n'))) *e = 0;
		// Prefer auth plain over auth login
		if (strstr(p, "PLAIN"))
			auth_type = AUTH_TYPE_PLAIN;
		else if (strstr(p, "LOGIN"))
			auth_type = AUTH_TYPE_LOGIN;
	}

	return 0;
}

static int smtp_one(const char *fname)
{
	char logout[1024];
	char buffer[1024];
	FILE *fp;
	int rc;

	*logout = 0;
	strlcat(logout, fname, sizeof(logout));

	rc = open_spool_file(fname, &fp);
	if (rc)
		return rc;

	rc = -1; // reset to failed

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		logmsg("socket: %s", strerror(errno));
		goto done;
	}

	int flags = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));

	struct sockaddr_in sock_name;
	memset(&sock_name, 0, sizeof(sock_name));
	sock_name.sin_family = AF_INET;
	sock_name.sin_addr.s_addr = smtp_addr;
	sock_name.sin_port = htons(smtp_port);

	if (connect(sock, (struct sockaddr *)&sock_name, sizeof(sock_name))) {
		logmsg("connect: %s", strerror(errno));
		goto done;
	}

#ifdef WANT_SSL
	if (use_ssl)
		if (openssl_open(sock, smtp_server))
			goto done;
#endif

	expect_status(sock, 220);

	if (send_ehlo(sock))
		goto done;

#ifdef WANT_SSL
	if (starttls) {
		if (send_str(sock, "STARTTLS\r\n", 220))
			goto done;

		if (openssl_open(sock, smtp_server))
			goto done;

		use_ssl = 1;

		// We have to send hello again
		if (send_ehlo(sock))
			goto done;
	}
#endif

	if (smtp_user) {
		if (auth_type == AUTH_TYPE_PLAIN) {
			char authplain[512];

			mkauthplain(smtp_user, smtp_passwd, authplain, sizeof(authplain));
			snprintf(buffer, sizeof(buffer), "AUTH PLAIN %s\r\n", authplain);
			if (send_str(sock, buffer, 235))
				goto done;
		} else if (auth_type == AUTH_TYPE_LOGIN) {
			char user64[128], passwd64[256];

			base64_encode(user64, sizeof(user64) - 2, (uint8_t *)smtp_user, strlen(smtp_user));
			base64_encode(passwd64, sizeof(passwd64) - 2, (uint8_t *)smtp_passwd, strlen(smtp_passwd));
			strcat(user64, "\r\n");
			strcat(passwd64, "\r\n");

			// We assume user then password... we should actually check reply
			if (send_str(sock, "AUTH LOGIN\r\n", 334))
				goto done;
			if (send_str(sock, user64, 334))
				goto done;
			if (send_str(sock, passwd64, 235))
				goto done;
		}
	}

	snprintf(buffer, sizeof(buffer), "MAIL FROM:<%s>\r\n", mail_from);
	if (send_str(sock, buffer, 250))
		goto done;

	char line[128], *p;
	int first_time = 1;
	int count = 0;
	while (fgets(line, sizeof(line), fp) && *line != '\n') {
		strtok(line, "\r\n");
		if ((p = strchr(line, '@'))) {
			strlcat(logout, " ", sizeof(logout));
			strlcat(logout, line, sizeof(logout));
			snprintf(buffer, sizeof(buffer), "RCPT TO:<%s>\r\n", line);
		} else {
			if (first_time) {
				first_time = 0;
				strlcat(logout, " ", sizeof(logout));
				strlcat(logout, mail_from, sizeof(logout));
				snprintf(buffer, sizeof(buffer), "RCPT TO:<%s>\r\n", mail_from);
			} else
				continue;
		}
		int n = send_str(sock, buffer, 250);
		if (n == 0)
			++count;
		else if (n < 0)
			goto done;
		else
			strlcat(logout, "(X)", sizeof(logout));
	}

	send_str(sock, "DATA\r\n", 354);

	send_body(sock, fp);

	send_str(sock, "QUIT\r\n", 221);

	logmsg("%s", logout);

	rc = 0; // success

done:
	fclose(fp);
    openssl_close();
	if (sock != -1)
		close(sock);
	return rc;
}
#endif

#define NEED_VAL if (!val) {					\
		logmsg("%s needs a value", key);		\
		continue;								\
	}

static void read_config(void)
{
	FILE *fp = fopen(CONFIG_FILE, "r");
	if (!fp) {
		logmsg(CONFIG_FILE ": %s", strerror(errno));
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
			logmsg("Unexpected key %s", key);
	}

	fclose(fp);

	if (!smtp_server) {
		logmsg("You must set smtp-server");
		exit(1);
	}
	// For now let's require mail-from
	if (!mail_from) {
		logmsg("You must set mail-from");
		exit(1);
	}
	if (smtp_user && !smtp_passwd) {
		logmsg("You must set smtp-user AND smtp-password");
		exit(1);
	}

#ifndef WANT_CURL
	char *server = strstr(smtp_server, "://");
	if (server) {
		server += 3;
#ifdef WANT_SSL
		if (strncmp(smtp_server, "smtps", 5) == 0) {
			smtp_port = 465;
			use_ssl = 1;
			if (debug) puts("Using SSL");
		}
#endif
		smtp_server = server;
	}

	struct hostent *host = gethostbyname(smtp_server);
	if (!host) {
		logmsg("Unable to get host %s", server);
		exit(1);
	}

	smtp_addr = *(uint32_t *)host->h_addr_list[0];

	if (gethostname(hostname, sizeof(hostname))) {
		logmsg("hostname: %s", strerror(errno));
		exit(1);
	}
#endif
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
	puts("usage: doorknob [-fds]\n"
		 "where: -d turns on debugging (enables foreground and stderr)\n"
		 "       -f keeps doorknob in foreground\n"
		 "       -h this help message\n"
		 "       -s use stderr rather than syslog");
	exit(1);
}

int main(int argc, char *argv[])
{
	int c;

	openlog("doorknob", 0, LOG_MAIL);

	while ((c = getopt(argc, argv, "DFdfhs")) != EOF)
		switch (c) {
		case 'D': // for backwards compatibility
		case 'd': debug = 1; foreground = 1; use_stderr = 1; break;
		case 'F': // for backwards compatibility
		case 'f': foreground = 1; break;
		case 'h': usage();
		case 's': use_stderr = 1; break;
		default: puts("Sorry!"); exit(1);
		}

	read_config();

	if (chdir(MAILDIR "/queue")) {
		logmsg(MAILDIR ": %s", strerror(errno));
		exit(1);
	}

	int fd = inotify_init();
	if (fd < 0) {
		logmsg("inotify_init: %s", strerror(errno));
		exit(1);
	}

	int watch = inotify_add_watch(fd, ".", IN_CLOSE_WRITE | IN_MOVED_TO);
	if (watch < 0) {
		logmsg("inotify_add_watch", strerror(errno));
		exit(1);
	}

	if (foreground == 0) {
		if (daemon(1, 0))
			logmsg("daemon: %s", strerror(errno));
	}

	struct pollfd ufd = { .fd = fd, .events = POLLIN };

	while (1) {
		DIR *dir = opendir(".");
		if (!dir) {
			logmsg("opendir: %s", strerror(errno));
			continue;
		}

		struct dirent *ent;
		while ((ent = readdir(dir)))
			if (*ent->d_name != '.')
				if (smtp_one(ent->d_name) >= 0)
					if (unlink(ent->d_name))
						logmsg("unlink %s: %s", strerror(errno));

		closedir(dir);

		// Timeout every hour
		if (poll(&ufd, 1, 3600000) == 1)
			read_event(fd);
	}

	return 0;
}
