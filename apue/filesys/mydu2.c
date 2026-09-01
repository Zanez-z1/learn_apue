#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

#define PATHSIZE  1024

static int64_t mydu(const char *path)
{
	char nextpath[PATHSIZE];
	struct stat res;
	struct dirent *dir_res;
	DIR *dir = NULL;
	int64_t sum = 0;

	if (lstat(path, &res) < 0)
	{
		perror("lstat");
		exit(1);
	}

	if (!S_ISDIR(res.st_mode))
		return res.st_blocks / 2;
	sum += res.st_blocks / 2;

	dir = opendir(path);
	if (dir == NULL)
	{
		perror("opendir");
		exit(1);
	}

	while (1)
	{
		errno = 0;
		dir_res = readdir(dir);
		if (dir_res == NULL)
		{
			if (errno != 0)
			{
				perror("readdir");
				exit(1);
			}
			break;
		}

		if (strcmp(dir_res->d_name, ".") == 0 ||
		    strcmp(dir_res->d_name, "..") == 0)
			continue;

		snprintf(nextpath, sizeof(nextpath), "%s/%s",
			 path, dir_res->d_name);
		sum += mydu(nextpath);
	}

	closedir(dir);
	return sum;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage:..\n");
		exit(1);
	}

	printf("%"PRId64"K\n", mydu(argv[1]));

	exit(0);
}
