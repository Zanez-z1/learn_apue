#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>


#define TIMESTRSIZE 1024
#define FMTSTRSIZE  1024

#define APPEND_FMT(s)  \
	strncat(fmtstr, (s), sizeof(fmtstr) - strlen(fmtstr) - 1)

int main(int argc, char *argv[])
{
	FILE *fp = stdout;
	time_t stamp;
	struct tm *tm;
	char timestr[TIMESTRSIZE];
	int ret;
	char fmtstr[FMTSTRSIZE];

	fmtstr[0] = '\0';
	stamp = time(NULL);
	tm = localtime(&stamp);
	while(1)
	{
		ret = getopt(argc, argv, "-H:MSy:md");
		if (ret < 0)
			break;

		switch(ret)
		{
			case 1:
				fp = fopen(argv[optind - 1],"w");
				if (fp == NULL)
				{
					perror("fopen()");
					fp = stdout;
				}
				break;
			case 'H':
				if (strcmp(optarg,"12") == 0)
					APPEND_FMT("%I(%P) ");
				else if (strcmp(optarg,"24") == 0)
					APPEND_FMT("%H ");
				else
					fprintf(stderr,"Invalid argument\n");
				break;
			case 'M':
				APPEND_FMT("%M ");
				break;
			case 'S':
				APPEND_FMT("%S ");
				break;
			case 'y':
				if (strcmp(optarg,"2") == 0)
					APPEND_FMT("%y ");
				else if (strcmp(optarg,"4") == 0)
					APPEND_FMT("%Y ");
				else
					fprintf(stderr,"Invalid argument\n");
				break;
			case 'm':
				APPEND_FMT("%m ");
				break;
			case 'd':
				APPEND_FMT("%d ");
				break;
			default:
				break;
		}
	}

	APPEND_FMT("\n");
	strftime(timestr,TIMESTRSIZE,fmtstr,tm);
	fputs(timestr,fp);

	if (fp != stdout)
		fclose(fp);

	exit(0);
}
