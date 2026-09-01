#include <stdio.h>
#include <stdlib.h>

static void bye(void)
{
	printf("time to say bye!\n");
}

int main()
{
	int ret;
	puts("Begin here");
	ret = atexit(bye);
	if (ret != 0)
	{
		fprintf(stderr, "cannot set exit function\n");
		exit(EXIT_FAILURE);
	}
	puts("End here");

	exit(EXIT_SUCCESS);
}
