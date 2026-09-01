#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TIMESTRSIZE 1024

int main(void)
{
	time_t stamp;
	struct tm *tm;
	char timestr[TIMESTRSIZE];

	stamp = time(NULL);
	if (stamp == (time_t)-1) {
		perror("time()");
		exit(1);
	}

	tm = localtime(&stamp);
	if (tm == NULL) {
		perror("localtime()");
		exit(1);
	}

	tm->tm_mday += 100;
	if (mktime(tm) == (time_t)-1) {
		fprintf(stderr, "mktime() failed\n");
		exit(1);
	}

	if (strftime(timestr, TIMESTRSIZE, "%Y-%m-%d %A", tm) == 0) {
		fprintf(stderr, "strftime() failed\n");
		exit(1);
	}

	puts(timestr);
	exit(0);
}
