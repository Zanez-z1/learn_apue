#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <glob.h>

#define PATHSIZE  1024
static bool is_loop(const char *path)
{
	const char *pos = NULL;
	pos = strrchr(path, '/');
	if (pos == NULL)
	{
		perror("pos");
		exit(1);
	}
	
	if (strcmp(pos + 1, ".") == 0 || strcmp(pos + 1, "..") == 0)
	{
		return true;
	}

	return false;
}
static int64_t mydu(const char *path)
{
	static struct stat res;
	glob_t glob_res;
	static char nextpath[PATHSIZE];
	int64_t sum = 0;
	if (lstat(path,&res) < 0)
	{
		perror("lstat");
		exit(1);
	}

	if(!S_ISDIR(res.st_mode))
		return res.st_blocks / 2;
	sum += res.st_blocks / 2;

	snprintf(nextpath, sizeof(nextpath), "%s/*", path);
	if(glob(nextpath,0,NULL,&glob_res))
	{
		perror("glob");
		exit(1);
	}
	
	snprintf(nextpath, sizeof(nextpath), "%s/.*", path);
	if(glob(nextpath,GLOB_APPEND,NULL,&glob_res))
	{
		perror("glob");
		exit(1);
	}
	
	for (int i = 0; i < glob_res.gl_pathc; i++)
	{
		if(!is_loop(glob_res.gl_pathv[i]))
			sum += mydu(glob_res.gl_pathv[i]);
	}
	globfree(&glob_res);
	return sum;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage:..\n");
		exit(1);
	}

	printf("%lldK\n", mydu(argv[1]));

	exit(0);
}
