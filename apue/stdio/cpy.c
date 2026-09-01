#include <stdio.h>
#include <stdlib.h>
#include <errno.h>


int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "Usage:%s <src> <des>\n", argv[0]); 
		exit(1);
	}

	FILE *fps,*fpd;
	int ret;
	
	fps = fopen(argv[1], "r");
	if (fps == NULL)
	{
		perror("open filed");
		exit(1);
	}

	fpd = fopen(argv[2], "w");
	if (fpd == NULL)
	{
		perror("open filed");
		fclose(fps);
		exit(1);
	}
	
	while (1)
	{
		ret = fgetc(fps);	
		if (ret == EOF)
			break;
		fputc(ret, fpd);
	}

	fclose(fpd);
	fclose(fps);

	exit(0);
}

