#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#define CPS     10
#define BUFSIZE	CPS 

static volatile int loop = 0;

static void alrm_handler(int sig)
{
	alarm(1);
	loop = 1;
}

int main(int argc, char **argv)
{
	int sfd,dfd = 1;
	char buf[BUFSIZE];
	int len,ret,pos;

	if (argc < 2)
	{
		fprintf(stderr,"Usage:...\n");
		exit(1);
	}

	signal(SIGALRM, alrm_handler);
	alarm(1);

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

		while (len = read(sfd,buf,BUFSIZE) < 0)
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
