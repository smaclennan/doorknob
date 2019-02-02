#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

#define QDIR MAILDIR"/queue"


int main(int argc, char *argv[])
{
	if (chdir(QDIR)) {
		perror("chdir " QDIR);
		exit(1);
	}

	DIR *dir = opendir(".");
	if (!dir) {
		perror("opendir " QDIR);
		exit(1);
	}

	struct dirent *ent;
	while ((ent = readdir(dir)))
		if (*ent->d_name != '.')
			puts(ent->d_name);

	if (closedir(dir)) {
		perror("closedir " QDIR);
		exit(1);
	}

	return 0;
}
