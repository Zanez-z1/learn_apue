#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define THRNUM  20 
#define FNAME   "/tmp/out"
#define BUFSIZE 1024

static pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;

static void *thr_add(void *)
{
    FILE *fp;
    char buf[BUFSIZE];
    
    fp = fopen(FNAME, "r+");
    if (fp == NULL)
    {
        perror("fopen");
        exit(1);
    }
    pthread_mutex_lock(&mut);
    fgets(buf, BUFSIZE, fp);

    fseek(fp, 0, SEEK_SET);

    sleep(1);
    fprintf(fp, "%d\n", atoi(buf) + 1);

    fclose(fp);
    pthread_mutex_unlock(&mut);

    pthread_exit(NULL);
}

int main()
{
    pthread_t tid[THRNUM];
    int i,err;

    for (i = 0; i < THRNUM; i++)
    {
        err = pthread_create(tid + i, NULL, thr_add, NULL);
        if (err)
        {
            fprintf(stderr,"pthread_create():%s",strerror(err));
            exit(1);
        }
    }

    for (i = 0; i < THRNUM; i++)
        pthread_join(tid[i], NULL);

    pthread_mutex_destroy(&mut);

    exit(0);
}
