#include <sys/timerfd.h>
#include <sys/epoll.h> /* EPOLLIN */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "timer.h"
#include "event.h"
#include "log.h"

#define SECOND(time) (M_SECOND(time) / 1000)
#define M_SECOND(time) ((time)*TIME_UNIT)
#define MICRO_SECOND(time) (M_SECOND(time)*1000)
#define N_SECOND(time) (M_SECOND(time) % 1000) * 1000000

enum {
    TIMER_OP_CREATE = 0,
    TIMER_OP_START,
    TIMER_OP_STOP,
    TIMER_OP_DESTROY,
};

static int
time_setting(int tmrfd, int time, int interval)
{
    struct itimerspec itmr;

    itmr.it_value.tv_sec = SECOND(time);
    itmr.it_value.tv_nsec = N_SECOND(time);
    itmr.it_interval.tv_sec = SECOND(interval);
    itmr.it_interval.tv_nsec = N_SECOND(interval);
    
    if(timerfd_settime(tmrfd, 0, &itmr, NULL)) {
        LOG_ERROR("timerfd_settime failed:%s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int
check_timer_param(struct timer_param *tp, int op)
{
    if(!tp) {
        return -1;
    }

    switch(op) {
        case TIMER_OP_CREATE:
            if(tp->epfd < 0 || !tp->tmr_hdl) {
                return -1;
            } 
            return 0;
        case TIMER_OP_START:
            if(tp->tmrfd < 0 || tp->time <= 0) {
                return -1;
            }
            return 0;
        case TIMER_OP_STOP:
            if(tp->tmrfd < 0) {
                return -1;
            }
            return 0;
        case TIMER_OP_DESTROY:
            if(tp->epfd < 0 || tp->tmrfd < 0) {
                return -1;
            }
            return 0;
        default:
            LOG_ERROR("invalid timer op[%d] value!\n", op);
            break;
    }

    return -1;
}

int
timer_creat(struct timer_param *tp)
{
    if(check_timer_param(tp, TIMER_OP_CREATE)) {
        return -1;
    }

    tp->tmrfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if(tp->tmrfd < 0) {
        LOG_ERROR("timerfd_create failed:%s\n", strerror(errno));
        return -1;
    }

    if(time_setting(tp->tmrfd, 0, 0)) {
        close(tp->tmrfd);
        tp->tmrfd = -1;
        return -1;
    }

    struct event_param ep;
    ep.fd = tp->tmrfd;
    ep.event = EPOLLIN;
    ep.evt_ctx = tp->tmr_ctx;
    ep.evt_hdl = tp->tmr_hdl;

    if(event_add(tp->epfd, &ep)) {
        LOG_ERROR("timer add failed!\n");
        close(tp->tmrfd);
        tp->tmrfd = -1;
        return -1;
    }

    return 0;
}

int
timer_start(struct timer_param *tp)
{
    if(check_timer_param(tp, TIMER_OP_START)) {
        return -1;
    }

    if(time_setting(tp->tmrfd, tp->time, tp->interval)) {
        return -1;
    }

    return 0;
}

int
timer_stop(struct timer_param *tp)
{
    if(check_timer_param(tp, TIMER_OP_STOP)) {
        return -1;
    }

    if(time_setting(tp->tmrfd, 0, 0)) {
        return -1;
    }

    return 0;
}

int
timer_destroy(struct timer_param *tp)
{
    if(timer_stop(tp)) {
        return -1;
    }

    struct event_param ep;
    memset(&ep, 0, sizeof(ep));
    ep.fd = tp->tmrfd;

    if(event_del(tp->epfd, &ep)) {
        LOG_ERROR("timer del failed!\n");
        return -1;
    }

    return 0;
}

int
timer_add(int epfd, struct timer_param *tp)
{
    tp->epfd = epfd;
    if(check_timer_param(tp, TIMER_OP_CREATE)) {
        LOG_ERROR("invalid timer param!\n");
        return -1;
    }

    int tmrfd;
    tmrfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if(tmrfd < 0) {
        LOG_ERROR("timerfd_create failed:%s\n", strerror(errno));
        return -1;
    }

    if(time_setting(tmrfd, tp->time, tp->interval)) {
        close(tmrfd);
        return -1;
    }

    struct event_param ep;
    ep.fd = tmrfd;
    ep.event = EPOLLIN;
    ep.evt_ctx = tp->tmr_ctx;
    ep.evt_hdl = tp->tmr_hdl;

    if(event_add(epfd, &ep)) {
        LOG_ERROR("timer add failed!\n");
        close(tmrfd);
        return -1;
    }

    return tmrfd;
}

int
timer_del(int epfd, int tmrfd)
{
    struct event_param ep;
    memset(&ep, 0, sizeof(ep));
    ep.fd = tmrfd;

    if(event_del(epfd, &ep)) {
        LOG_ERROR("timer del failed!\n");
        return -1;
    }

    return 0;
}

