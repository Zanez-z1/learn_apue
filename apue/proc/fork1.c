#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main()
{
	pid_t pid;
	printf("[%d] Begin!\n",getpid());

	fflush(NULL);
	pid = fork();
	if (pid < 0)
	{
		perror("fork()");
		exit(1);
	}

	if (pid == 0)
	{
		printf("[Child] my pid is %d,my parent is %d\n",getpid(),getppid());
	}
	else
	{
		printf("[Parent] my pid is %d,my child pid is %d\n",getpid(),pid);
		wait(NULL);
	}

	printf("[%d] End!\n",getpid());

	getchar();
	exit(0);
}
