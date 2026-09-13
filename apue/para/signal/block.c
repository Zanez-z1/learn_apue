#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

void sig_handler(int sig)
{
    write(1,"!",1);
}

int main()
{

    sigset_t set,saveset;
    signal(SIGINT,sig_handler);
    sigemptyset(&set);
    sigaddset(&set,SIGINT);
    sigprocmask(SIG_UNBLOCK,&set,&saveset);
    for (int i = 0; i < 1000; i++)
    {
        sigprocmask(SIG_BLOCK,&set,NULL);
        for (int j = 0; j < 5; j++)
        {
            write(1,"*",1);
            sleep(1);
        }
        write(1,"\n",1);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
    }
    sigprocmask(SIG_SETMASK, &saveset, NULL);
    exit(0);
}
