#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#define LEFT    30000000
#define RIGHT   30000200
#define THRNUM  4 


static int num;
static pthread_mutex_t mut_num = PTHREAD_MUTEX_INITIALIZER;


struct thr_arg_st
{
    int n;
};

static void *thr_primer(void *p)
{
    int i,j,mark;

    while(1)
    {
        /*线程抢锁*/
        pthread_mutex_lock(&mut_num);
        /*此处while循环判断作用和main线程类似*/
        while (num == 0)
        {
            pthread_mutex_unlock(&mut_num);
            sched_yield();
            pthread_mutex_lock(&mut_num);
        }

        if (num == -1)
        {
            pthread_mutex_unlock(&mut_num);
            break;
        }

        i = num;
        num = 0;
        pthread_mutex_unlock(&mut_num);

        mark = 1;
        for (j = 2; j < i/2; j++)
        {
            if (i % j == 0)
            {
                mark = 0;
                break;
            }
        }
        if (mark)
            printf("[%d]%d is a primer\n", ((struct thr_arg_st *)p)->n, i);

    }

    pthread_exit(p);

}

int main()
{
    int i,err;

    pthread_t tid[THRNUM];
    struct thr_arg_st *p;
    void *ptr;

    for(i = 0; i < THRNUM; i++)
    {
        p = malloc(sizeof(*p));
        if (p == NULL)
        {
            perror("malloc");
            exit(1);
        }
        p->n = i;

        err = pthread_create(tid + i, NULL, thr_primer, p);
        if (err)
        {
            fprintf(stderr,"pthread_create():%s\n",strerror(err));
            exit(1);
        }
    }

    for (i = LEFT; i <= RIGHT; i++)
    {
        pthread_mutex_lock(&mut_num);

        while (num != 0)
        {
            //如果num不等于0，解锁让别的线程抢锁
            pthread_mutex_unlock(&mut_num);
            //出让调度器给其他线程，避免当前线程反复抢锁，而其他线程拿不到锁
            sched_yield();
            //当别的线程操作完之后，再抢锁判断num是否为0，如果仍然不是0，循环进行以上操作
            pthread_mutex_lock(&mut_num);
        }
        /*如果是0,则下发本次任务,下发完毕之后让出锁*/
        num = i;
        pthread_mutex_unlock(&mut_num);
    }
    /*保证最后一次下发的任务被取走*/
    pthread_mutex_lock(&mut_num);
    while(num != 0)
    {
        pthread_mutex_unlock(&mut_num);
        sched_yield();
        pthread_mutex_lock(&mut_num);
    }
    /*设置num = -1,标志所有任务已下发*/
    num = -1;
    pthread_mutex_unlock(&mut_num);

    for(i = 0; i < THRNUM; i++)
    {
        pthread_join(tid[i], &ptr);
        free(ptr);
    }

    pthread_mutex_destroy(&mut_num);

    exit(0);
}
