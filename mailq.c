#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>


int main(int argc, char *argv[])
{
	if (chdir(MAILDIR)) {
		perror(MAILDIR);
		exit(1);
	}
	if (chdir("queue")) {
		fprintf(stderr, MAILDIR "/queue: %s\n", strerror(errno));
		exit(1);
	}

	DIR *dir = opendir(".");
	if (!dir) {
		perror("opendir");
		exit(1);
	}

	struct dirent *ent;
	while ((ent = readdir(dir)))
		if (*ent->d_name != '.')
			puts(ent->d_name);

	closedir(dir);

	return 0;
}
