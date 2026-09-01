#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#define FNAME  		"/tmp/out"
#define BUFSIZE 	1024

static volatile sig_atomic_t running = 1;

void handler(int sig)
{
	running = 0;
}

int main(int argc, char **argv)
{
	struct tm *tm;
	FILE *fp;
	char buf[BUFSIZE];
	int count = 0;
	time_t stamp;

	if(signal(SIGINT, handler) == SIG_ERR)
	{
		perror("signal");
		exit(1);
	}

	fp = fopen(FNAME, "a+");
	if (fp == NULL)
	{
		perror("fopen");
		exit(1);
	}
	
	while(fgets(buf,BUFSIZE,fp) !=NULL)
	{
		count++;
	}

	while(running)
	{
		time(&stamp);
		tm = localtime(&stamp);
		fflush(fp);
		fprintf(fp,"%-4d%d-%d-%d %02d:%02d:%02d\n",++count,	\
		  tm->tm_year + 1900,tm->tm_mon + 1,tm->tm_mday,	\
		  tm->tm_hour,tm->tm_min,tm->tm_sec);

		sleep(1);

	}
	printf("\n");
	fclose(fp);
	exit(0);
}
