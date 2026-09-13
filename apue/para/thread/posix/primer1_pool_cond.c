#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define LEFT    30000000
#define RIGHT   30000200
#define THRNUM  4

/* 0：槽位为空；正数：待领取的任务；-1：结束 */
static int num = 0;

static pthread_mutex_t mut_num = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_job = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cond_empty = PTHREAD_COND_INITIALIZER;

struct thr_arg_st
{
    int n;
};

static void *thr_primer(void *p)
{
    struct thr_arg_st *arg = p;
    int i, j, mark;

    while (1)
    {
        pthread_mutex_lock(&mut_num);

        /* 没有任务时，释放锁并休眠；返回时已重新获得锁 */
        while (num == 0)
            pthread_cond_wait(&cond_job, &mut_num);

        if (num == -1)
        {
            pthread_mutex_unlock(&mut_num);
            break;
        }

        /* 领取任务，通知主线程可以下发下一个任务 */
        i = num;
        num = 0;
        pthread_cond_signal(&cond_empty);

        pthread_mutex_unlock(&mut_num);

        /* 在锁外计算，使其他线程能够同时领取和处理任务 */
        mark = 1;

        for (j = 2; j <= i / j; j++)
        {
            if (i % j == 0)
            {
                mark = 0;
                break;
            }
        }

        if (mark)
            printf("[%d] %d is a prime\n", arg->n, i);
    }

    return p;
}

int main(void)
{
    int i, err;
    pthread_t tid[THRNUM];
    struct thr_arg_st *p;
    void *ptr;

    for (i = 0; i < THRNUM; i++)
    {
        p = malloc(sizeof(*p));
        if (p == NULL)
        {
            perror("malloc()");
            exit(1);
        }

        p->n = i;

        err = pthread_create(&tid[i], NULL, thr_primer, p);
        if (err != 0)
        {
            fprintf(stderr, "pthread_create(): %s\n", strerror(err));
            free(p);
            exit(1);
        }
    }

    for (i = LEFT; i <= RIGHT; i++)
    {
        pthread_mutex_lock(&mut_num);

        /* 等待上一个任务被领取 */
        while (num != 0)
            pthread_cond_wait(&cond_empty, &mut_num);

        num = i;

        /* 一个任务，通知一个工作线程 */
        pthread_cond_signal(&cond_job);

        pthread_mutex_unlock(&mut_num);
    }

    pthread_mutex_lock(&mut_num);

    /* 等待最后一个任务被领取，避免覆盖它 */
    while (num != 0)
        pthread_cond_wait(&cond_empty, &mut_num);

    num = -1;

    /* 唤醒所有等待任务的线程，让它们退出 */
    pthread_cond_broadcast(&cond_job);

    pthread_mutex_unlock(&mut_num);

    /* 正在计算的线程会先完成任务，再检查结束标志 */
    for (i = 0; i < THRNUM; i++)
    {
        err = pthread_join(tid[i], &ptr);
        if (err != 0)
        {
            fprintf(stderr, "pthread_join(): %s\n", strerror(err));
            exit(1);
        }

        free(ptr);
    }

    pthread_cond_destroy(&cond_job);
    pthread_cond_destroy(&cond_empty);
    pthread_mutex_destroy(&mut_num);

    return 0;
}
