#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

void handler(int sig)
{
	printf("get SIGINT!do nothing!\n");
}

int main()
{
	// signal(SIGINT,SIG_IGN);
	signal(SIGINT,handler);
	for(int i = 0; i < 10; i++)
	{
		write(1,"*",1);
		sleep(1);
	}

	exit(0);
}
