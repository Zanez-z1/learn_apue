#include <asm-generic/errno-base.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#define tty1     "/dev/tty11"
#define tty2     "/dev/tty12"
#define BUFSIZE  1024

enum 
{
    STATE_W,
    STATE_R,
STATE_AUTO,
    STATE_T,
    STATE_E,
};

struct fsm_st
{
    int state;
    int sfd;
    int dfd;
    int len;
    int pos;
    char buf[BUFSIZE];
    char *errstr;
};


int max(int a, int b)
{
    if (a > b)
        return a;
    return b;
}

static void fsm_driver(struct fsm_st *fsm)
{
    int ret;
    /*开始逐个判断每个状态*/
    switch (fsm->state)
    {
        case STATE_R:
            /*将读取到的长度存储在结构体里，方便write时使用*/
            fsm->len = read(fsm->sfd, fsm->buf, BUFSIZE);
            /*len等于零，代表读取结束，进入STATE_T结束状态*/
            if (fsm->len == 0)
            {
                fsm->state = STATE_T;
            }
            /*否则如果len小于零，则需要分情况讨论，如果errno是EAGAIN，代表本次没读到数据，重新开始读
                * 否则进入STATE_E错误状态*/
            else if (fsm->len < 0)
            {
                if (errno == EAGAIN)
                {
                    fsm->state = STATE_R;
                }
                else
                {
                    fsm->errstr = "read()";
                    fsm->state = STATE_E;
                }
            }
            /*如果len大于零，说明读到了数据，初始化pos为零，并且将状态切换为STATE_W写状态*/
            else
            {
                fsm->pos = 0;
                fsm->state = STATE_W;
            }
            break;

        case STATE_W:
            /*state为STATE_W,进入写状态*/
            ret = write(fsm->dfd, fsm->buf + fsm->pos, fsm->len);
            /*此处错误判断同read，分假错和真错，分别进行处理*/
            if (ret < 0)
            {
                if (errno == EAGAIN)
                {
                    fsm->state = STATE_W;
                }
                else
                {
                    fsm->errstr = "write()";
                    fsm->state = STATE_E;
                }

            }
            /*ret大于等于0，计算写入了多少字节，如果一次没写完，则重新进入写状态继续写*/
            else 
            {
                fsm->pos += ret;
                fsm->len -= ret;
                if (fsm->len > 0)
                {
                    fsm->state = STATE_W;
                }
                else
                {
                    fsm->state = STATE_R;
                }
            }
            break;

        case STATE_E:
            perror(fsm->errstr);
            fsm->state = STATE_T;
            break;

        case STATE_T:
            break;

        default:
            abort();
            break;
    }
}

static void relay(int fd1, int fd2)
{
    int fd1_save, fd2_save; 
    struct fsm_st fsm12, fsm21;
    struct pollfd fds[2]; 

    fd1_save = fcntl(fd1, F_GETFL);
    fcntl(fd1, F_SETFL, fd1_save | O_NONBLOCK);

    fd2_save = fcntl(fd2, F_GETFL);
    fcntl(fd2, F_SETFL, fd2_save | O_NONBLOCK);

    fsm12.state = STATE_R;
    fsm12.sfd = fd1;
    fsm12.dfd = fd2;

    fsm21.state = STATE_R;
    fsm21.sfd = fd2;
    fsm21.dfd = fd1;

    fds[0].fd = fd1;
    fds[1].fd = fd2;

    while (fsm12.state != STATE_T || fsm21.state != STATE_T)
    {
        /*设置监听事件*/
        fds[0].events = 0;
        fds[1].events = 0;

        if (fsm12.state == STATE_R)
            fds[0].events |= POLLIN;
        if (fsm21.state == STATE_W)
            fds[0].events |= POLLOUT;
        if (fsm12.state == STATE_W)
            fds[1].events |= POLLOUT;
        if (fsm21.state == STATE_R)
            fds[1].events |= POLLIN;
        /*开始监听*/
        if (fsm12.state < STATE_AUTO || fsm21.state < STATE_AUTO)
        {
            while (poll(fds, 2, -1) < 0)
            {
                if (errno == EINTR)
                    continue;

                perror("poll()");
                exit(1);
            }

        }
        /*查看结果*/
        if (fds[0].revents & POLLIN || fds[1].revents & POLLOUT || fsm12.state > STATE_AUTO)
        {
            fsm_driver(&fsm12);
        }
        if (fds[0].revents & POLLOUT || fds[1].revents & POLLIN || fsm21.state > STATE_AUTO)
        {
            fsm_driver(&fsm21);
        }
    }
    
    fcntl(fd1, F_SETFL, fd1_save);
    fcntl(fd2, F_SETFL, fd2_save);
}

int main()
{
    int fd1,fd2;

    fd1 = open(tty1, O_RDWR);
    if (fd1 < 0)
    {
        perror("open");
        exit(1);
    }
    write(fd1, "TTY1\n", 5);

    fd2 = open(tty2, O_RDWR | O_NONBLOCK);
    if (fd2 < 0)
    {
        perror("open");
        exit(1);
    }
    write(fd2, "TTY2\n", 5);

    relay(fd1, fd2);

    close(fd2);
    close(fd1);

    exit(0);
}
