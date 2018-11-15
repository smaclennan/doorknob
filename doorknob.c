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
#include <curl/curl.h>

static char *smtp_server;
static char *smtp_user;
static char *smtp_passwd;
static char *mail_from;
static int starttls;
static int rewrite_from;

static int foreground;
static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (foreground)
		vprintf(fmt, ap);
	else
		vsyslog(LOG_INFO, fmt, ap);
	va_end(ap);
}

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
		syslog(LOG_INFO, log);
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

// SAM This is not optimal... curl lets us keep the connection open
// and send multiple emails at once.

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
			}
			return strlen(buffer);
		}
		return 0;
	}

	return fread(buffer, size, nitems, fp);
}

size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	size *= nitems;

	printf(">>> %.*s", (int)size, buffer);

	return size;
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

	char line[100], *p; // SAM FIXME
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
	looking_for_from = rewrite_from;
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
	curl_easy_setopt(curl, CURLOPT_READDATA, fp);
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);

#if 0
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif

#if 0
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // SAM DBG
	// curl_easy_setopt(curl, CURLOPT_STDERR, file pointer);
#endif

	/* Send the message */
	res = curl_easy_perform(curl);
	logsmtp(fname, recipients, res);

done:
	fclose(fp);
	curl_slist_free_all(recipients);
	curl_easy_cleanup(curl);

	return res;
}

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
			// SAM this should be at least trivially encrypted
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

int main(int argc, char *argv[])
{
	int c;

	read_config();

	while ((c = getopt(argc, argv, "F")) != EOF)
		switch (c) {
		case 'F': foreground = 1; break;
		default: puts("Sorry!"); exit(1);
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
