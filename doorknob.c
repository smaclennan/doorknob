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
#include <pwd.h>
#include <sys/stat.h>
#include <sys/inotify.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

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
static int use_ssl;

static short smtp_port = 25;
static uint32_t smtp_addr; // ipv4 only
static char hostname[HOST_NAME_MAX + 1];

#ifndef WANT_SSL
#define ssl_open(s, h) -1
#define ssl_read(b, c) -1
#define ssl_write(b, c) -1
#define ssl_close()
#endif

#ifdef __QNX__
#include <sys/slog2.h>
#include <sys/procmgr.h>

static void logging_init(void)
{
	slog2_buffer_t buffer_handle;
	slog2_buffer_set_config_t buffer_config = {
		.num_buffers = 1,
		.buffer_set_name = "doorknob",
		.verbosity_level = SLOG2_INFO,
		.buffer_config[0].buffer_name = "doorknob",
		.buffer_config[0].num_pages = 1,
	};

	if (slog2_register(&buffer_config, &buffer_handle, 0)) {
		fprintf(stderr, "Error registering slog2 buffer!\n");
		exit(1);
	}

	slog2_set_default_buffer(buffer_handle);
}

static void sys_log(const char *msg)
{
	slog2c(NULL, 0, SLOG2_INFO, msg);
}
#else
static void logging_init(void)
{
	openlog("doorknob", 0, LOG_MAIL);
}

static void sys_log(const char *msg)
{
	syslog(LOG_INFO, "%s", msg);
}
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
		sys_log(msg);
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

static int read_socket(int sock, void *buf, int count)
{
	if (use_ssl)
		return ssl_read(buf, count);
	else
		return read(sock, buf, count);
}

static int write_socket(int sock, const void *buf, int count)
{
	if (use_ssl)
		return ssl_write(buf, count);
	else
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

	if (debug)
		printf("S: %s", reply);

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
	if (debug)
		printf("C: %s", str);

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

	strconcat(buffer, sizeof(buffer), "EHLO ", hostname, "\r\n", NULL);
	if (send_str(sock, buffer, 250))
		return -1;

	// For starttls this may change so reset
	auth_type = 0;

	p = strstr(reply, "250-AUTH");
	if (p) {
		e = strchr(p, '\n');
		if (e)
			*e = 0;
		// Prefer auth plain over auth login
		if (strstr(p, "PLAIN"))
			auth_type = AUTH_TYPE_PLAIN;
		else if (strstr(p, "LOGIN"))
			auth_type = AUTH_TYPE_LOGIN;
	}

	return 0;
}

static int open_and_connect(void)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		logmsg("socket: %s", strerror(errno));
		return -1;
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
		close(sock);
		return -1;
	}

	// starttls defers the ssl_open
	if (use_ssl && !starttls) {
		if (ssl_open(sock, smtp_server)) {
			close(sock);
			return -1;
		}
	}

	return sock;
}

static int auth_user(int sock, char *buffer, size_t bufsize)
{
	if (auth_type == AUTH_TYPE_PLAIN) {
		char authplain[512];

		mkauthplain(smtp_user, smtp_passwd, authplain, sizeof(authplain));
		strconcat(buffer, bufsize, "AUTH PLAIN ", authplain, "\r\n", NULL);
		if (send_str(sock, buffer, 235))
			return -1;
	} else if (auth_type == AUTH_TYPE_LOGIN) {
		char user64[128], passwd64[256];

		base64_encode(user64, sizeof(user64) - 2, (uint8_t *)smtp_user, strlen(smtp_user));
		base64_encode(passwd64, sizeof(passwd64) - 2, (uint8_t *)smtp_passwd, strlen(smtp_passwd));
		strcat(user64, "\r\n");
		strcat(passwd64, "\r\n");

		// We assume user then password... we should actually check reply
		if (send_str(sock, "AUTH LOGIN\r\n", 334))
			return -1;
		if (send_str(sock, user64, 334))
			return -1;
		if (send_str(sock, passwd64, 235))
			return -1;
	}

	return 0;
}

static int start_starttls(int sock)
{
	use_ssl = 0; // turn it off for first hello

	expect_status(sock, 220);

	if (send_ehlo(sock))
		return -1;

	if (send_str(sock, "STARTTLS\r\n", 220))
		return -1;

	if (ssl_open(sock, smtp_server))
		return -1;

	use_ssl = 1;

	// We have to send hello again
	return send_ehlo(sock);
}

static int smtp_one(const char *fname)
{
	char logout[1024];
	char buffer[1024];
	FILE *fp;
	int rc;

	strlcpy(logout, fname, sizeof(logout));

	rc = open_spool_file(fname, &fp);
	if (rc) {
		logmsg("open %s: %s", fname, strerror(errno));
		return rc;
	}

	rc = -1; // reset to failed

	int sock = open_and_connect();
	if (sock == -1) {
		goto done;
	}

	if (starttls) {
		if (start_starttls(sock))
			goto done;
	} else {
		expect_status(sock, 220);

		if (send_ehlo(sock))
			goto done;
	}

	if (smtp_user)
		auth_user(sock, buffer, sizeof(buffer));

	strconcat(buffer, sizeof(buffer), "MAIL FROM:<", mail_from, ">\r\n", NULL);
	if (send_str(sock, buffer, 250))
		goto done;

	char line[128], *p;
	int first_time = 1;
	int count = 0;
	while (fgets(line, sizeof(line), fp) && *line != '\n') {
		strtok(line, "\r\n");
		p = strchr(line, '@');
		if (p) {
			strlcat(logout, " ", sizeof(logout));
			strlcat(logout, line, sizeof(logout));
			strconcat(buffer, sizeof(buffer), "RCPT TO:<", line, ">\r\n", NULL);
		} else {
			if (first_time) {
				first_time = 0;
				strlcat(logout, " ", sizeof(logout));
				strlcat(logout, mail_from, sizeof(logout));
				strconcat(buffer, sizeof(buffer), "RCPT TO:<", mail_from, ">\r\n", NULL);
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
	ssl_close();
	if (sock != -1)
		close(sock);
	return rc;
}

#define NEED_VAL do {							\
		if (!val) {								\
			logmsg("%s needs a value", key);	\
			continue;							\
		}										\
	} while (0)

static void read_config(void)
{
	FILE *fp = fopen(CONFIGFILE, "r");
	if (!fp) {
		logmsg(CONFIGFILE ": %s", strerror(errno));
		exit(1);
	}

	char line[128];
	while (fgets(line, sizeof(line), fp)) {
		if (!strrchr(line, '\n')) {
			logmsg("Config file line to long");
			exit(1);
		}
		if (*line == '#')
			continue;
		char *key = strtok(line, " \t\r\n");
		char *val = strtok(NULL, "\r\n");
		if (!key)
			continue; // empty line
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
		} else if (strcmp(key, "starttls") == 0) {
#ifndef WANT_SSL
			logmsg("starttls not supported");
			exit(1);
#endif
			starttls = 1;
		} else if (strcmp(key, "rewrite-from") == 0)
			rewrite_from = 1;
		else if (strcmp(key, "cert") == 0) {
#ifdef WANT_SSL
			NEED_VAL;
			if (ssl_read_cert(val)) {
				logmsg("Bad cert file %s", val);
				exit(1);
			}
#endif
		} else
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

	char *server = strstr(smtp_server, "://");
	if (server) {
		server += 3;
		if (strncmp(smtp_server, "smtps", 5) == 0) {
#ifndef WANT_SSL
			logmsg("smtps not supported");
			exit(1);
#endif
			smtp_port = 465;
			use_ssl = 1;
			if (debug)
				puts("Using SSL");
		}
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
}

// This is really to get around the read() return warning.
// The event is just a trigger, we don't care what it says.
static int read_event(int fd)
{
	uint8_t event[sizeof(struct inotify_event) + NAME_MAX + 1];
	return read(fd, event, sizeof(event));
}

static void _usage(void)
{
	puts("usage: doorknob [-fds]\n"
		 "where: -d turns on debugging (enables foreground and stderr)\n"
		 "       -f keeps doorknob in foreground\n"
		 "       -h this help message\n"
		 "       -s use stderr rather than syslog\n"
		 "\n"
		 "Note: You must have an " CONFIGFILE " configured for your system.\n"
		 "      A sample doorknob.conf is provided with the source.");
	exit(1);
}

int main(int argc, char *argv[])
{
	int c, no_change = 0;

	logging_init();

	while ((c = getopt(argc, argv, "CDFdfhs")) != EOF)
		switch (c) {
		case 'C':
			no_change = 1;
			break;
		case 'D': // for backwards compatibility
		case 'd':
			debug = 1;
			foreground = 1;
			use_stderr = 1;
			break;
		case 'F': // for backwards compatibility
		case 'f':
			foreground = 1;
			break;
		case 'h':
			_usage();
		case 's':
			use_stderr = 1;
			break;
		default:
			puts("Sorry! Maybe try -h for help?");
			exit(1);
		}

	if (chdir(MAILDIR "/queue")) {
		logmsg(MAILDIR "/queue: %s", strerror(errno));
		exit(1);
	}

	DIR *dir = opendir(".");
	if (!dir) {
		logmsg("opendir: %s", strerror(errno));
		exit(1);
	}

	int fd = inotify_init();
	if (fd < 0) {
		logmsg("inotify_init: %s", strerror(errno));
		exit(1);
	}

	int watch = inotify_add_watch(fd, ".", IN_CLOSE_WRITE | IN_MOVED_TO);
	if (watch < 0) {
		logmsg("inotify_add_watch: %s", strerror(errno));
		exit(1);
	}

	read_config();

	/* Do this after inotify setup and reading config */
	if (no_change == 0) {
		struct passwd *pw = getpwnam(DOORKNOBUSER);
		if (!pw) {
			logmsg(DOORKNOBUSER " user does not exist");
			exit(1);
		}

		if (setgid(pw->pw_gid)) {
			logmsg("setgid %d: %s", pw->pw_gid, strerror(errno));
			exit(1);
		}

		if (setuid(pw->pw_uid)) {
			logmsg("setuid %d: %s", pw->pw_uid, strerror(errno));
			exit(1);
		}
	}

	if (foreground == 0) {
#ifdef __QNX__
		// daemon call causes slog2 to stop working
		if (procmgr_daemon(0, PROCMGR_DAEMON_NOCLOSE | PROCMGR_DAEMON_NOCHDIR))
			logmsg("daemon: %s", strerror(errno));
#else
		if (daemon(1, 0))
			logmsg("daemon: %s", strerror(errno));
#endif
	}

	struct pollfd ufd = { .fd = fd, .events = POLLIN };

	while (1) {
		struct dirent *ent;
		int timeout = 3600000; // one hour

		rewinddir(dir);
		while ((ent = readdir(dir)))
			if (*ent->d_name != '.') {
				if (smtp_one(ent->d_name) >= 0) {
					if (unlink(ent->d_name))
						logmsg("unlink %s: %s", strerror(errno));
				} else
					// More aggressive timeout if smtp_one failed
					timeout = 60000; // one minute
			}

		if (poll(&ufd, 1, timeout) == 1)
			read_event(fd);
	}

	return 0;
}
