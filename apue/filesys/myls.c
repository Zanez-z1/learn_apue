#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

int main(int argc, char **argv)
{
	const char *path = ".";
	struct dirent *dir_res;
	DIR *dir;
	int show_all = 0;
	int opt;

	while ((opt = getopt(argc, argv, "a")) != -1)
	{
		switch (opt)
		{
			case 'a':
				show_all = 1;
				break;
			default:
				fprintf(stderr, "Usage: %s [-a] [directory]\n", argv[0]);
				exit(1);
		}
	}

	if (optind < argc)
		path = argv[optind];

	if (optind + 1 < argc)
	{
		fprintf(stderr, "Usage: %s [-a] [directory]\n", argv[0]);
		exit(1);
	}

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
				closedir(dir);
				exit(1);
			}
			break;
		}

		if (!show_all && dir_res->d_name[0] == '.')
			continue;

		puts(dir_res->d_name);
	}

	closedir(dir);
	exit(0);
}
