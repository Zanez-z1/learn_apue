#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>


int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr,"Usage:...\n");
		exit(1);
	}
	int fd;
	fd = open(argv[1],O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0)
	{
		perror("open");
		exit(1);
	}
	
	lseek(fd,5l*1024l*1024l*1024l - 1,SEEK_SET);

	write(fd,"",1);

	exit(0);
}

