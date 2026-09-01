#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
	at_addtimer(2, f1, "bbb");
	at_addtimer(5, f2, "aaa");

	/* TODO: 让进程活着等定时器触发,比如 while (1) pause(); */

	exit(0);
}
