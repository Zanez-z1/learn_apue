#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>

int main(int argc, char **argv)
{
	struct passwd *psd;
	uid_t uid;
	errno = 0;

	if (argc < 2)
	{
		fprintf(stderr, "Usage:<%s><uid>\n",argv[0]);
		exit(1);
	}
	uid = (uid_t)atoi(argv[1]);
	psd = getpwuid(uid);
	if (psd == NULL)
	{
		if (errno != 0)
		{
			perror("getpwuid");
			exit(1);
		}
		fprintf(stderr,"Uid not found:%u\n",uid);
		exit(1);
	}

	printf("username :%s\n",psd->pw_name);
	return 0;
}
