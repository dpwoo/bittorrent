#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "mempool.h"
#include "btype.h"
#include "event.h"
#include "fd_hash.h"
#include "log.h"

int
event_create(void)
{
    int epfd = epoll_create(32);
    if(epfd < 0) {
        LOG_ERROR("epoll_create failed:%s\n", strerror(errno));
        return -1;
    }
    return epfd;
}

static int
check_event_param(int epfd, struct event_param *ep)
{
    if(epfd < 0 || !ep || ep->fd < 0) {
        return -1;
    }

    if( !(ep->event & (EPOLLIN|EPOLLOUT)) ) {
        return -1;
    }

    if(!ep->evt_hdl) {
        return -1;
    }
   
    return 0;
}

int
event_add(int epfd, struct event_param *ep)
{
    if(check_event_param(epfd, ep)) {
        LOG_ERROR("invalid event param!\n");
        return -1;
    }   

    struct event_param *eparam;
    eparam = GMALLOC(sizeof(*eparam));
    if(!eparam) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }
    *eparam = *ep;

    if(fd_hash_add(ep->fd, eparam)) {
        GFREE(eparam);
        return -1;
    }

    struct epoll_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.events = ep->event;
    evt.data.fd = ep->fd;

    if(epoll_ctl(epfd, EPOLL_CTL_ADD, ep->fd, &evt)) {
        LOG_ERROR("epoll_add failed:%s\n", strerror(errno));
        fd_hash_del(ep->fd);
        GFREE(eparam);
        return -1;
    }

    return 0;
}

int
event_mod(int epfd, struct event_param *ep)
{
    if(check_event_param(epfd, ep)) {
        return -1;
    }

    struct event_param *eparam;
    eparam = (struct event_param *)fd_hash_find(ep->fd);
    if(!eparam) {
        fprintf(stderr, "no found fd:%d in fd hash!\n", ep->fd);
        return -1;
    }
    *eparam = *ep;

    struct epoll_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.events = ep->event;
    evt.data.fd = ep->fd;

    if(epoll_ctl(epfd, EPOLL_CTL_MOD, ep->fd, &evt)) {
        fprintf(stderr, "event mod failed:%s\n", strerror(errno));
        return -1;
    }

    return 0;
}

int
event_del(int epfd, struct event_param *ep)
{
    if(epfd < 0 || !ep || ep->fd < 0) {
        return -1;
    }

    struct event_param *eparam;
    eparam = (struct event_param *)fd_hash_find(ep->fd);
    if(!eparam) {
        LOG_ERROR("no found fd:%d in fd hash!\n", ep->fd);
        return -1;
    }

    fd_hash_del(ep->fd);

    GFREE(eparam);

    struct epoll_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.events = ep->event;
    evt.data.fd = ep->fd;

    if(epoll_ctl(epfd, EPOLL_CTL_DEL, ep->fd, &evt)) {
        LOG_ERROR("event del failed:%s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void
dump_event(int event)
{
    LOG_DEBUG("event:%x\n", event);
    LOG_DEBUG("EPOLLIN:%x\n", EPOLLIN);
    LOG_DEBUG("EPOLLOUT:%x\n", EPOLLOUT);
    LOG_DEBUG("EPOLLRDHUP:%x\n", EPOLLRDHUP);
    LOG_DEBUG("EPOLLHUP:%x\n", EPOLLHUP);
    LOG_DEBUG("EPOLLERR:%x\n", EPOLLERR);
}

static int
event_dispatch(int epfd, int event, int fd)
{
    struct event_param *ep;

    ep = fd_hash_find(fd);
    if(!ep) {
        LOG_ERROR("dispatch: no found fd:%d in fd hash!\n", fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return -1;
    }

#if 0
    dump_event(event);
#endif

    if(ep->evt_hdl) {
        (*ep->evt_hdl)(event, ep->evt_ctx);
    }

    return 0;
}

int
event_loop(int epfd)
{
    struct epoll_event evts[1024];

    while(1) {
        int nevt = epoll_wait(epfd, evts, sizeof(evts)/sizeof(evts[0]), -1);
        if(nevt < 0) {
            if(errno == EINTR) {
                continue;
            }
            LOG_FATAL("epoll_wait failed:%s\n", strerror(errno));
            return -1;
        }

        int i;
        for(i = 0; i < nevt; i++) {
            event_dispatch(epfd, evts[i].events, evts[i].data.fd);
        }
    }

    return 0;
}

