#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>


int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage:...\n");
		exit(1);
	}

	int fd1,fd2;
	int len,ret;
	char buf[1024];
	int new_len;
	int line = 1;
	char c;
	/*fd1 used for read*/
	fd1 = open(argv[1], O_RDONLY, 0644);
	if (fd1 < 0)
	{
		perror("open");
		exit(1);
	}
	/*fd2 used for read and write*/
	fd2 = open(argv[1], O_RDWR, 0644);
	if (fd2 < 0)
	{
		perror("open");
		exit(1);
	}

	/*calculate sizeof 10th line*/
	len = lseek(fd1,0,SEEK_END);
	lseek(fd1,0,SEEK_SET);
	while (line < 10 && read(fd1,&c,1) > 0)
	{
		if (c == '\n')
			line++;
	}
	int start = lseek(fd1,0,SEEK_CUR);
	while (read(fd1,&c,1) > 0)
	{
		if (c == '\n')
			break;
	}
	int end = lseek(fd1,0,SEEK_CUR);
	int line10_len = end - start;
	new_len = len - line10_len;
	/*use fd1 read 11th row*/
	/*use fd1 read 11th row*/
	lseek(fd1,end,SEEK_SET);
	lseek(fd2,start,SEEK_SET);
	while(1)
	{
		len = read(fd1,buf,sizeof(buf));	
		if (len < 0)
		{
			perror("read");
			break;
		}
		if (len == 0)
		{
			break;
		}
		write(fd2,buf,len);
	}

	ftruncate(fd2, new_len);

	exit(0);
}
