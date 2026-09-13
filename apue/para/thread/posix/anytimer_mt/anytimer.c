#include <asm-generic/errno-base.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "anytimer.h"

#define ANYTIMER_MAX    1024

enum state{
	running,
	executing,
	over,
	canceled,
};

struct timer_st
{
	int sec;			/* 剩余秒数,减到 0 就触发 */
	void (*func)(void *);
	void *arg;
	enum state flag;
	pthread_mutex_t mut;
	pthread_cond_t cond;
};

static struct timer_st *job[JOB_MAX];	/* 注册表:handler 靠它找到所有定时器 */
static pthread_mutex_t mut_job = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_job = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_t tid;

/* 每 tick 被信号打断一次:遍历 job[],各定时器 sec--,到期的调 func(arg) 并回收槽位 */
static void* thr_handler(void *p)
{

	/* TODO: 遍历 job[] 数组 */
	while (1)
	{
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		pthread_mutex_lock(&mut_job);
		for (int i = 0; i < JOB_MAX; i++)
		{
			struct timer_st *me = job[i];	
			if (me == NULL)
				continue;

			pthread_mutex_lock(&job[i]->mut);
			if (me->flag == running)
			{
				me->sec--;
				if (me->sec <= 0)
				{
					me->flag = executing;
					/*调用回调函数之前解锁，否则回调函数里反复加锁*/
					pthread_mutex_unlock(&me->mut);
					pthread_mutex_unlock(&mut_job);
					me->func(me->arg);

					/*回调结束之后，重新加锁*/
					pthread_mutex_lock(&mut_job);
					pthread_mutex_lock(&me->mut);

					job[i]->flag = over;
					pthread_cond_broadcast(&me->cond);
				}
			}
			pthread_mutex_unlock(&me->mut);
		}
		pthread_mutex_unlock(&mut_job);

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		sleep(1);
	}
	return NULL;
}

static void module_unload(void)	/* 前置声明:module_load 里要 atexit 它 */
{
	pthread_cancel(tid);
	pthread_join(tid, NULL);

	pthread_mutex_lock(&mut_job);
	for (int i = 0; i < JOB_MAX; i++)
	{
		if (job[i] != NULL)
		{
			pthread_cond_destroy(&job[i]->cond);
			pthread_mutex_destroy(&job[i]->mut);
			free(job[i]);
			job[i] = NULL;
		}

	}
	pthread_mutex_unlock(&mut_job);
	pthread_mutex_destroy(&mut_job);
	pthread_cond_destroy(&cond_job);
}

static void module_load(void)
{
	int err;
	
	err = pthread_create(&tid, NULL, thr_handler, NULL);
	if (err)
	{
		fprintf(stderr, "pthread_create():%s", strerror(err));
		exit(1);
	}

	atexit(module_unload);
}

static int get_free_pos_unlocked(void)
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
	int pos,err;
	struct timer_st *me;
	
	pthread_once(&once, module_load);

	/* TODO: 参数校验(sec >= 0、func != NULL),非法返回 -EINVAL */
	if (sec < 0 || func == NULL)
		return -EINVAL;

	me = malloc(sizeof(*me));
	if (me == NULL)
	{
		return -ENOMEM;
	}

	pthread_mutex_lock(&mut_job);
	pos = get_free_pos_unlocked();
	if (pos < 0)
	{
		pthread_mutex_unlock(&mut_job);
		free(me);
		return -ENOSPC;		/* 槽位已满 */
	}

	me->sec = sec;
	me->func = func;
	me->arg = arg;
	me->flag = running;
	err = pthread_mutex_init(&me->mut, NULL);
	if (err !=0)
	{
		pthread_mutex_unlock(&mut_job);
		free(me);
		return -err;
	}
	err = pthread_cond_init(&me->cond, NULL);
	if (err != 0)
	{
		pthread_mutex_destroy(&me->mut);
		pthread_mutex_unlock(&mut_job);
		free(me);
		return -err;
	}
	/* TODO: 存入 job[pos],返回 pos 作为定时器 id */
	job[pos] = me;
	pthread_mutex_unlock(&mut_job);

	return pos;
}

int at_canceltimer(int id)
{
	struct timer_st *me;
	/* TODO: 校验 id 范围(0 <= id < JOB_MAX)且 job[id] != NULL */
	if (id < 0 || id >= JOB_MAX)
	{
		return -EINVAL;
	}

	pthread_mutex_lock(&mut_job);
	me = job[id];
	if (me == NULL)
	{
		pthread_mutex_unlock(&mut_job);
		return -EINVAL;
	}
	pthread_mutex_lock(&me->mut);
	if (me->flag == canceled)
	{
		pthread_mutex_unlock(&me->mut);
		pthread_mutex_unlock(&mut_job);
		return -ECANCELED;
	}

	if (me->flag == over || me->flag == executing)
	{
		pthread_mutex_unlock(&me->mut);
		pthread_mutex_unlock(&mut_job);
		return -EBUSY;
	}

	me->flag = canceled;
	pthread_cond_broadcast(&job[id]->cond);

	pthread_mutex_unlock(&me->mut);
	pthread_mutex_unlock(&mut_job);

	return 0;
}

int at_waittimer(int id)
{
	struct timer_st *me;

	if (id < 0 || id >= JOB_MAX)
	{
		return -EINVAL;
	}

	/*找到定时器，并拿到定时器里的锁*/	
	pthread_mutex_lock(&mut_job);

	me = job[id];
	if (me == NULL)
	{
		pthread_mutex_unlock(&mut_job);
		return -EINVAL;
	}
	pthread_mutex_lock(&me->mut);
	/*确保拿到定时器里的锁，再解锁全局锁*/
	pthread_mutex_unlock(&mut_job);

	while (me->flag == running || me->flag == executing)
	{
		pthread_cond_wait(&me->cond, &me->mut);
	}
	pthread_mutex_unlock(&me->mut);

	pthread_mutex_lock(&mut_job);
	job[id] = NULL;
	pthread_mutex_unlock(&mut_job);
	
	pthread_mutex_destroy(&me->mut);
	pthread_cond_destroy(&me->cond);

	free(me);
	
	return 0;
}
