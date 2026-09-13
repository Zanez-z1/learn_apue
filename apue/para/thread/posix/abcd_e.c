#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define THRNUM  4

static int turn = 0;
static pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

int next(int num)
{
    int next = num + 1;
    if (next == THRNUM)
        return 0;
    return next;
}

static void *thr_func(void *p)
{
    int n = (int)p;
    char c = 'a' + n;


    while (1)
    {
        pthread_mutex_lock(&mut);
        while (turn != n)
            pthread_cond_wait(&cond, &mut);
        write(1,&c,1);
        turn = next(turn);
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mut);
    }

    pthread_exit(NULL);
}

int main()
{
    pthread_t tid[THRNUM];
    int i,err;

    for (i = 0; i < THRNUM; i++)
    {
        err = pthread_create(tid + i, NULL, thr_func, (void *)i);
        if (err)
        {
            fprintf(stderr, "create:%s\n", strerror(err));
            exit(1);
        }
    }
    
    alarm(3);

    for (i = 0; i < THRNUM; i++)
    {
        pthread_join(tid[i],NULL);
    }

    pthread_mutex_destroy(&mut);
    pthread_cond_destroy(&cond);
    
    exit(0);
}
