#include <asm-generic/errno-base.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include "anytimer.h"

#define ANYTIMER_MAX    1024

enum state{
	running,
	over,
	canceled,
};

struct timer_st
{
	int sec;			/* 剩余秒数,减到 0 就触发 */
	void (*func)(void *);
	void *arg;
	enum state flag;
};

typedef typeof(void (int))  *sighandler_t;
static struct timer_st *job[JOB_MAX];	/* 注册表:handler 靠它找到所有定时器 */
static int inited = 0;
static sighandler_t alrm_handler_save;

/* 每 tick 被信号打断一次:遍历 job[],各定时器 sec--,到期的调 func(arg) 并回收槽位 */
static void alrm_handler(int sig)
{
	alarm(1);
	/* TODO: 遍历 job[] 数组 */
	for (int i = 0; i < JOB_MAX; i++)
	{
		if (job[i] != NULL && job[i]->flag == running)
		{
			job[i]->sec--;
			if (job[i]->sec <= 0)
			{
				job[i]->flag = over;
				job[i]->func(job[i]->arg);
			}
		}
	}
}

static void module_unload(void)	/* 前置声明:module_load 里要 atexit 它 */
{
	signal(SIGALRM, alrm_handler_save);
	alarm(0);

	for (int i = 0; i < JOB_MAX; i++)
		free(job[i]);
}

static void module_load(void)
{
	/* TODO: 保存旧的 SIGALRM handler(供 unload 恢复) */
	alrm_handler_save = signal(SIGALRM, alrm_handler);

	alarm(1);

	atexit(module_unload);
}

static int get_free_pos(void)
{
	/* TODO: 遍历 job[] 找第一个 NULL 槽位,返回下标;满了返回 -1 */
	for (int i = 0; i < JOB_MAX; i++)
	{
		if (job[i] == NULL)
		{
			return i;
		}
	}
	return -1;
}

int at_addtimer(int sec, void (*func)(void *), void *arg)
{
	int pos;
	struct timer_st *me;
	if (!inited)
	{
		module_load();
		inited = 1;
	}

	/* TODO: 参数校验(sec >= 0、func != NULL),非法返回 -EINVAL */
	if (sec < 0 || func == NULL)
		return -EINVAL;

	pos = get_free_pos();
	if (pos < 0)
		return -ENOSPC;		/* 槽位已满 */
	
	/* TODO: malloc 一个 struct timer_st,填 sec/func/arg */
	me = malloc(sizeof(*me));
	if (me == NULL)
	{
		return -ENOMEM;
	}

	me->sec = sec;
	me->func = func;
	me->arg = arg;
	me->flag = running;
	/* TODO: 存入 job[pos],返回 pos 作为定时器 id */
	job[pos] = me;

	return pos;
}

int at_canceltimer(int id)
{
	/* TODO: 校验 id 范围(0 <= id < JOB_MAX)且 job[id] != NULL */
	if (id < 0 || id >= JOB_MAX || job[id] == NULL)
	{
		return -EINVAL;
	}
	if (job[id]->flag == canceled)
		return -ECANCELED;

	if (job[id]->flag == over)
		return -EBUSY;

	job[id]->flag = canceled;

	return 0;
}

int at_waitttimer(int id)
{
	struct timer_st *me;

	if (id < 0 || id >= JOB_MAX || job[id] == NULL)
	{
		return -EINVAL;
	}

	me = job[id];
	
	while (me->flag == running)
		pause();

	job[id] = NULL;
	free(me);
	
	return 0;
}
