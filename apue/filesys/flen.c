#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>


static off_t flen(char *filename)
{
	struct stat st;
	if(stat(filename,&st) < 0)
	{
		perror("stat");
		exit(1);
	}

	return st.st_size;
}


int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage:...\n");
		exit(0);
	}

	printf("filesize:%ld\n",flen(argv[1]));
	exit(0);
}
