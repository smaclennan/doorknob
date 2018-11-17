/* sendmail.c - sendmail replacement for doorknob
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
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <sys/time.h>

#define MAX_QNAME (20 + 6 + 10 + 3) // includes the NULL

static char tmp_path[MAX_QNAME + 4];
static char real_path[MAX_QNAME + 6];

static int create_tmp_file(void)
{
	char tmp_file[MAX_QNAME];
	struct timeval now;

	gettimeofday(&now, NULL);
	snprintf(tmp_file, sizeof(tmp_file), "%lu.%06ld.%d",
			 now.tv_sec, now.tv_usec, getpid());

	snprintf(tmp_path, sizeof(tmp_path), "tmp/%s", tmp_file);
	snprintf(real_path, sizeof(real_path), "queue/%s", tmp_file);

	/* Yes, it must be world writable for doorknob. This file is
	 * protected by the directory permissions. */
	int fd = creat(tmp_path, 0666);
	if (fd < 0) {
		perror(tmp_path);
		exit(1);
	}

	return fd;
}

static size_t total_len, total_write;
static int my_write(int fd, const void *buf, size_t len)
{
	total_len += len;
	int n = write(fd, buf, len);
	total_write += n;
	return n;
}

static void add_to_buffer(const char *line, ssize_t len, char **buffer, size_t *blen)
{
	*buffer = realloc(*buffer, *blen + len);
	if (!*buffer) {
		fputs("Out of buffer space!\n", stderr);
		exit(1);
	}
	memcpy(*buffer + *blen, line, len);
	*blen += len;
}

static void out_one(const char *to, int fd)
{
	int len = strlen(to);
	my_write(fd, to, len);
	my_write(fd, "\n", 1);
}

static void output_to(char *line, int fd)
{
	char *to, *p;

	line += 3;
	if (*line != ':') ++line; // Bcc
	while ((to = strtok(line, ",\r\n"))) {
		line = NULL;
		while (isspace(*to)) ++to;
		if ((p = strchr(to, '<'))) {
			to = p + 1;
			if ((p = strchr(to, '>')))
				*p = 0;
			out_one(to, fd);
		} else
			out_one(to, fd);
	}
}

static void look_for_to(int fd)
{
	char *line = NULL, *buffer = NULL;
	size_t len = 0, blen = 0, count = 0;
	ssize_t n;

	while ((n = getline(&line, &len, stdin)) != -1) {
		add_to_buffer(line, n, &buffer, &blen);

		if (strncasecmp(line, "To:", 3) == 0 ||
			strncasecmp(line, "Cc:", 3) == 0 ||
			strncasecmp(line, "Bcc:", 4) == 0) {
			// found one
			output_to(line, fd);
			++count;
		} else if (*line == '\n' || *line == '\r') {
			// end of header
			if (count == 0)
				goto invalid;
			my_write(fd, "\n", 1); // end of recipients
			my_write(fd, buffer, blen);
			free(line);
			free(buffer);
			return;
		}
	}
invalid:
	fputs("Invalid header\n", stderr);
	exit(1);
}

int main(int argc, char *argv[])
{
	int c, evil_t = 0;

	while ((c = getopt(argc, argv, "it")) != EOF)
		switch (c) {
		case 'i': break;
		case 't': evil_t = 1; break;
		}

	if (optind == argc && !evil_t) {
		fprintf(stderr, "No recipients\n");
		exit(1);
	}

	struct passwd *pw = getpwuid(getuid());
	if (!pw) {
		fprintf(stderr, "You don't exist!\n");
		exit(1);
	}

	if (chdir(MAILDIR)) {
		perror(MAILDIR);
		exit(1);
	}

	char *hostname = getenv("HOSTNAME");
	if (!hostname) {
		hostname = malloc(100);
		gethostname(hostname, 100);
	}

	int fd = create_tmp_file();

	if (evil_t)
		look_for_to(fd);
	else {
		// Write out the recipients
		for (int i = optind; i < argc; ++i) {
			my_write(fd, argv[i], strlen(argv[i]));
			my_write(fd, "\n", 1);
		}
		my_write(fd, "\n", 1);
	}

	// Write out the from
	char from[128], *p;
	snprintf(from, sizeof(from), "From: %s", pw->pw_gecos);
	if ((p = strchr(from, ','))) *p = 0;
	int n = strlen(from);
	n += snprintf(from + n, sizeof(from) - n, " <%s@%s>\n", pw->pw_name, hostname);
	my_write(fd, from, n);

	/* Read the email and write to file */
	char buff[4096];
	while ((n = read(0, buff, sizeof(buff))) > 0)
		if (write(fd, buff, n) != n)
			goto write_error;

	if (n)
		goto read_error;

	if (total_len != total_write)
		goto write_error;

	if (fsync(fd))
		goto write_error;

	close(fd);

	if (rename(tmp_path, real_path)) {
		fprintf(stderr, "Unable to rename %s to %s\n", tmp_path, real_path);
		unlink(tmp_path);
		exit(1);
	}

	return 0;

write_error:
	fprintf(stderr, "%s: write error\n", tmp_path);
	close(fd);
	unlink(tmp_path);
	return 1;

read_error:
	fprintf(stderr, "%s: read error\n", tmp_path);
	close(fd);
	unlink(tmp_path);
	return 1;
}
