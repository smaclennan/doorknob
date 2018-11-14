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

static FILE *create_tmp_file(void)
{
	char tmp_file[MAX_QNAME];
	struct timeval now;

	gettimeofday(&now, NULL);
	snprintf(tmp_file, sizeof(tmp_file), "%lu.%06ld.%d",
			 now.tv_sec, now.tv_usec, getpid());

	snprintf(tmp_path, sizeof(tmp_path), "tmp/%s", tmp_file);
	snprintf(real_path, sizeof(real_path), "queue/%s", tmp_file);

	FILE *fp = fopen(tmp_path, "w");
	if (!fp) {
		perror(tmp_path);
		exit(1);
	}

	return fp;
}


int main(int argc, char *argv[])
{
	int c;

	while ((c = getopt(argc, argv, "it")) != EOF)
		switch (c) {
		case 'i': break;
		case 't':
			fputs("Sorry, we don't support -t yet.\n", stderr);
			exit(1);
		}

	if (optind == argc) {
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

	char hostname[100];
	gethostname(hostname, sizeof(hostname));

	// SAM I think it should be an fd after all...
	FILE *fp = create_tmp_file();

	// Write out the recipients
	for (int i = optind; i < argc; ++i)
		fprintf(fp, "%s\n", argv[i]);
	fputc('\n', fp);

	// Write out the from
	char from[128], *p;
	snprintf(from, sizeof(from), "From: %s", pw->pw_gecos);
	if ((p = strchr(from, ','))) *p = 0;
	int n = strlen(from);
	fprintf(fp, "%s <%s@%s>\n", from, pw->pw_name, hostname);

	/* Read the email and write to file */
	char buff[4096];
	while ((n = read(0, buff, sizeof(buff))) > 0)
		if (fwrite(buff, n, 1, fp) != 1)
			goto write_error;

	fwrite("\r\n.\r\n", 5, 1, fp);
	fflush(fp);
	if (ferror(fp))
		goto write_error;

	if (n)
		goto read_error;

	fclose(fp);

	if (rename(tmp_path, real_path)) {
		fprintf(stderr, "Unable to rename %s to %s\n", tmp_path, real_path);
		unlink(tmp_path);
		exit(1);
	}

	return 0;

write_error:
	fprintf(stderr, "%s: write error\n", tmp_path);
	fclose(fp);
	unlink(tmp_path);
	return -1;

read_error:
	fprintf(stderr, "%s: read error\n", tmp_path);
	fclose(fp);
	unlink(tmp_path);
	return -1;

	return 0;
}
