#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include "anytimer.h"

#define ANYTIMER_MAX    1024

struct timer_st
{
	int sec;			/* 剩余秒数,减到 0 就触发 */
	void (*func)(void *);
	void *arg;
};

static struct timer_st *job[ANYTIMER_MAX];	/* 注册表:handler 靠它找到所有定时器 */
static int inited = 0;

static void module_unload(void);	/* 前置声明:module_load 里要 atexit 它 */

/* 每 tick 被信号打断一次:遍历 job[],各定时器 sec--,到期的调 func(arg) 并回收槽位 */
static void alrm_handler(int sig)
{
	(void)sig;
	/* TODO: 遍历 job[] 数组 */
	/* TODO: 非空槽位 sec--,减到 0 则调用 func(arg),然后释放并置空槽位 */
}

static void module_load(void)
{
	/* TODO: 保存旧的 SIGALRM handler(供 unload 恢复) */
	signal(SIGALRM, alrm_handler);
	/* TODO: setitimer(ITIMER_REAL, ...) 起 1 秒周期的定时器 */

	atexit(module_unload);
}

static void module_unload(void)
{
	/* TODO: 关掉定时器、恢复旧 handler */
	/* TODO: free 所有非空槽位 */
}

static int get_free_pos(void)
{
	/* TODO: 遍历 job[] 找第一个 NULL 槽位,返回下标;满了返回 -1 */
	return -1;
}

int at_addtimer(int sec, void (*func)(void *), void *arg)
{
	int pos;

	if (!inited)
	{
		module_load();
		inited = 1;
	}

	/* TODO: 参数校验(sec >= 0、func != NULL),非法返回 -EINVAL */

	pos = get_free_pos();
	if (pos < 0)
		return -ENOSPC;		/* 槽位已满 */

	/* TODO: malloc 一个 struct timer_st,填 sec/func/arg */
	/* TODO: 存入 job[pos],返回 pos 作为定时器 id */

	return -1;
}

int at_canceltimer(int id)
{
	(void)job;	/* 占位防止未使用警告,实现后删除本行 */
	/* TODO: 校验 id 范围(0 <= id < ANYTIMER_MAX)且 job[id] != NULL */
	/* TODO: free(job[id]); job[id] = NULL; 返回 0 */

	return -1;
}
