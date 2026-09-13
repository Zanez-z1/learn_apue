#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "anytimer.h"

static void f1(void *p)
{
	printf("f1():%s\n", (char *)p);
}

static void f2(void *p)
{
	printf("f2():%s\n", (char *)p);
}

int main(void)
{
	int job1,job2,job3;

	job1 = at_addtimer(2, f1, "bbb");
	if (job1 < 0)
	{
		fprintf(stderr,"at_addtimer():%s",strerror(-job1));
		exit(1);
	}

	job2 = at_addtimer(5, f2, "aaa");
	if (job2 < 0)
	{
		fprintf(stderr,"at_addtimer():%s",strerror(-job2));
		exit(1);
	}

	job3 = at_addtimer(7, f1, "ccc");
	if (job3 < 0)
	{
		fprintf(stderr,"at_addtimer():%s",strerror(-job3));
		exit(1);
	}

	/* TODO: 让进程活着等定时器触发,比如 while (1) pause(); */
	while(1)
	{
		write(1,".",1);
		sleep(1);
	}
	at_canceltimer(job3);
	at_canceltimer(job2);
	at_canceltimer(job1);

	exit(0);
}
