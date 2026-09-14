#ifndef ANYTIMER_H__
#define ANYTIMER_H__

#define JOB_MAX    1024
/*
 * anytimer:单一定时器多路复用的"任意个"定时器库
 *
 * at_addtimer(sec, func, arg):注册一个定时器,sec 秒后调用 func(arg)
 *                             返回定时器 id(>= 0),失败返回负的错误码
 * return   >= 0         成功，返回任务id
 *          == -EINVAL  失败，参数非法
 *          == -ENOSPC  失败，数组满
 *          == -ENOMEM  失败，内存空间不足
 *
 * at_canceltimer(id):         取消一个定时器
 *
 * return   == 0            成功，指定任务取消
 *          == -EINVAL      失败，参数非法
 *          == -EBUSY       失败，指定任务
 *          == -ECANCELED   失败，指定任务重复取消
 *
 *
 * at_waittimer(id):          回收任务
 *
 * return   == 0            成功，指定任务成功释放
 *          == -EINVAL      失败，参数非法
 */
int at_addtimer(int sec, void (*func)(void *), void *arg);
int at_canceltimer(int id);
int at_waittimer(int id);
#endif
