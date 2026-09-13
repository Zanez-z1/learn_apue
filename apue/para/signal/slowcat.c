#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

#define CPS     10
#define BUFSIZE	CPS 

static volatile int loop = 0;

static void alrm_handler(int sig)
{
	loop = 1;
}

int main(int argc, char **argv)
{
	int sfd,dfd = 1;
	char buf[BUFSIZE];
	int len,ret,pos;

    struct itimerval val;

	if (argc < 2)
	{
		fprintf(stderr,"Usage:...\n");
		exit(1);
	}


	signal(SIGALRM, alrm_handler);

    val.it_interval.tv_sec = 1;
    val.it_interval.tv_usec = 0;
    val.it_value.tv_sec = 1;
    val.it_value.tv_usec = 0;

    if (setitimer(ITIMER_REAL, &val, NULL) < 0)
    {
        perror("setitimer()");
        exit(1);
    }

	do
	{
		sfd = open(argv[1], O_RDONLY);
		if (sfd < 0)
		{
			if (errno != EINTR)
			{
				perror("open()");
				exit(1);
			}
		}
	}while (sfd < 0);

	while(1)
	{
		while(!loop)
			pause();
		loop = 0;

		while ((len = read(sfd,buf,BUFSIZE)) < 0)
		{
			if (errno == EINTR)
				continue;
			perror("read()");
			break;
		}
		if (len == 0)
			break;

		pos = 0;
		while(len > 0)
		{
			ret = write(dfd, buf + pos, len);
			if (ret < 0)
			{
				if (errno == EINTR)
					continue;
				perror("write()");
				exit(1);
			}
			pos += ret;
			len -= ret;
		}
	}

	close(sfd);

}
