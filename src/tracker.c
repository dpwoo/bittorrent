#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "btype.h"
#include "tracker.h"
#include "timer.h"
#include "log.h"

int
tracker_reset_members(struct tracker *tr)
{
    if(tr->sockid > 0) {
        close(tr->sockid);
    }

    if(tr->tmrfd > 0) {
        close(tr->tmrfd);
    }

    tr->sockid = -1;
    tr->tmrfd = -1;
    tr->connect_cnt = 0;
    tr->state = TRACKER_STATE_NONE;

    return 0;
}

int
tracker_destroy_timer(struct tracker *tr)
{
    if(tr->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.epfd = tr->tsk->epfd;
    tp.tmrfd = tr->tmrfd;
    
    if(timer_destroy(&tp)) {
        LOG_ERROR("peer destroy timer failed!\n");
        return -1;
    }

    close(tr->tmrfd);
    tr->tmrfd = -1;

    return 0;
}

int
tracker_stop_timer(struct tracker *tr)
{
    if(tr->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.tmrfd = tr->tmrfd;
    
    if(timer_stop(&tp)) {
        LOG_ERROR("tracker stop timer failed!\n");
        return -1;
    }

    return 0;
}

int
tracker_start_timer(struct tracker *tr, int time)
{
    if(tr->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.tmrfd = tr->tmrfd;
    tp.time = time;
    tp.interval = 0;

    if(timer_start(&tp)) {
        LOG_ERROR("tracker start timer failed!\n");
        return -1;
    }

    return 0;
}

int
tracker_create_timer(struct tracker *tr, event_handle_t handle)
{
    if(tr->tmrfd >= 0) {
        LOG_ALARM("tracker tmrfd is [%d] when create timer!\n", tr->tmrfd);
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.epfd = tr->tsk->epfd;
    tp.tmr_hdl = handle;
    tp.tmr_ctx = tr;

    if(timer_creat(&tp)) {
        LOG_ERROR("tracker create timer failed!\n");
        return -1;
    }

    tr->tmrfd = tp.tmrfd;

    return 0;
}

int
tracker_add_event(int event, struct tracker *tr, event_handle_t handle)
{
    struct event_param ep;
    ep.fd = tr->sockid;
    ep.event = event;
    ep.evt_ctx = tr;
    ep.evt_hdl = handle;

    if(event_add(tr->tsk->epfd, &ep)) {
        LOG_ERROR("tracker add event failed!\n");
        return -1;
    }

    return 0;
}

int
tracker_mod_event(int event, struct tracker *tr, event_handle_t handle)
{
    struct event_param ep;
    ep.fd = tr->sockid;
    ep.event = event; 
    ep.evt_ctx = tr;
    ep.evt_hdl = handle;

    if(event_mod(tr->tsk->epfd, &ep)) {
        LOG_ERROR("tracker mod event failed!\n");
        return -1;
    }

    return 0;
}

int
tracker_del_event(struct tracker *tr)
{
    struct event_param ep;

    memset(&ep, 0, sizeof(ep));
    ep.fd = tr->sockid;

    if(event_del(tr->tsk->epfd, &ep)) {
        LOG_ERROR("tracker del event failed!\n");
    }

    return 0;
}

