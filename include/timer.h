#ifndef TIMER_H
#define TIMER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "event.h"

/* 10 ms per unit */
#define TIME_UNIT 10

struct timer_param {
    int epfd, tmrfd;
    int time, interval;
    event_handle_t tmr_hdl;
    void *tmr_ctx;
};

int timer_creat(struct timer_param *tp);

int timer_start(struct timer_param *tp);

int timer_stop(struct timer_param *tp);

int timer_destroy(struct timer_param *tp);

int timer_add(int epfd, struct timer_param *tp);

int timer_del(int epfd, int tmrfd);

#ifdef __cplusplus
extern "C" } 
#endif

#endif
