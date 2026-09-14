#include <asm-generic/errno-base.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define TTY1     "/dev/tty11"
#define TTY2     "/dev/tty12"
#define TTY3     "/dev/tty9"
#define TTY4     "/dev/tty10"



int main()
{
    int fd1, fd2, fd3, fd4;
	int job1, job2;

    fd1 = open(tty1, O_RDWR);
    if (fd1 < 0)
    {
        perror("open");
        exit(1);
    }
    write(fd1, "TTY1\n", 5);

    fd2 = open(tty2, O_RDWR | O_NONBLOCK);
    if (fd2 < 0)
    {
        perror("open");
        exit(1);
    }
    write(fd2, "TTY2\n", 5);
	
	job1 = rel_addjob();

	close(fd4);
	close(fd3);
    close(fd2);
    close(fd1);

    exit(0);
}
