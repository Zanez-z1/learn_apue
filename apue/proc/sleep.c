#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main()
{
	pid_t pid;

	fflush(NULL);

	pid = fork();
	if (pid < 0)
	{
		perror("fork()");
		exit(0);
	}

	if (pid == 0)
	{
		execl("/usr/bin/sleep","sleep","10",NULL);
		perror("execl");
		exit(1);
	}

	wait(NULL);
	exit(0);
}
