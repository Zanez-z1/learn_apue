#ifndef ANYTIMER_H__
#define ANYTIMER_H__

/*
 * anytimer:单一定时器多路复用的"任意个"定时器库
 *
 * at_addtimer(sec, func, arg):注册一个定时器,sec 秒后调用 func(arg)
 *                             返回定时器 id(>= 0),失败返回负的错误码
 * at_canceltimer(id):         取消一个定时器
 */
int at_addtimer(int sec, void (*func)(void *), void *arg);
int at_canceltimer(int id);

#endif
