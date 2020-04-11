#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* Send an email to one recipient via sendmail.
 * Subject can be NULL or part of the body. If subject exists it is
 * assumed that the body has no header elements so it adds the to and date.
 */
int sendmail(const char *to, const char *subject, const char *body)
{
	char cmd[80];
	char *p = strchr(to, '<');
	if (p) {
		snprintf(cmd, sizeof(cmd), "sendmail %s", p + 1);
		p = strchr(cmd, '>');
		if (p)
			*p = 0;
	} else
		snprintf(cmd, sizeof(cmd), "sendmail %s", to);

	FILE *pfp = popen(cmd, "w");
	if (!pfp)
		return -1;

	if (subject) {
		fprintf(pfp, "To: %s\n", to);
		fprintf(pfp, "Subject: %s\n", subject);

		time_t now = time(NULL);
		struct tm *tm = localtime(&now);
		char date[42];
		strftime(date, sizeof(date), "Date: %a, %d %b %Y %T %z\n\n", tm);
		if (date[11] == '0')
			memmove(date + 11, date + 12, 27 + 1);
		fputs(date, pfp);
	}

	fputs(body, pfp);

	int ret = pclose(pfp);
	if (ret == -1)
		return -1;

	return !WIFEXITED(ret) || WEXITSTATUS(ret);
}
