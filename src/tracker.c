#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "btype.h"
#include "socket.h"
#include "http.h"
#include "event.h"
#include "tracker.h"
#include "timer.h"
#include "log.h"
#include "torrent.h"
#include "peer.h"
#include "utils.h"

static int reset_tracker_members(struct tracker *tr);
static int tracker_timer_add(struct tracker *tr);
static int tracker_timer_del(struct tracker *tr);
static int tracker_timeout_handle(int event, struct tracker *tr);

static int tracker_add_event(int event, struct tracker *tr);
static int tracker_mod_event(int event, struct tracker *tr);
static int tracker_del_event(struct tracker *tr);

static int tracker_event_handle_connecting(int event, struct tracker *tr);
static int tracker_event_handle_sendreq(int event, struct tracker *tr);
static int tracker_event_handle_waitrsp(int event, struct tracker *tr);
static int tracker_event_handle(int event, void *evt);
static int tracker_parser_response(struct tracker *tr, char *rspbuf, int buflen);

static int tracker_connect(struct torrent_task *tsk);
static int tracker_try_announce(struct torrent_task *tsk);

static int
reset_tracker_members(struct tracker *tr)
{
    free(tr->tp.reqpath);
    free(tr->tp.host);
    free(tr->tp.port);
    memset(&tr->tp, 0, sizeof(tr->tp));

    if(tr->ai) {
        freeaddrinfo(tr->ai);
        tr->ai = NULL;
    }

    if(tr->sockid > 0) {
        close(tr->sockid);
        tr->sockid = -1;
    }

    if(tr->tmrfd > 0) {
        close(tr->tmrfd);
        tr->tmrfd = -1;
    }
    
    tr->state = TRACKER_STATE_NONE;

    return 0;
}

static int
tracker_timeout_handle(int event, struct tracker *tr)
{
    LOG_INFO("handle (%s:%s) timeout\n", tr->tp.host, tr->tp.port);

    long long tmrbuf;
    if(read(tr->tmrfd, &tmrbuf, sizeof(tmrbuf)) != sizeof(tmrbuf)) {
        LOG_ALARM("read timer fd failed1\n");
    }

    tracker_timer_del(tr);

    tracker_del_event(tr);

    tracker_try_announce(tr->tsk);

    return 0;
}

static int
tracker_timer_add(struct tracker *tr)
{
    struct timer_param tp;
    tp.time = tr->state == TRACKER_STATE_CONNECTING ? 500 : 1000;
    tp.interval = 0;
    tp.tmr_ctx = tr;
    tp.tmr_hdl = (event_handle_t)tracker_timeout_handle;

    if((tr->tmrfd = timer_add(tr->tsk->epfd, &tp)) < 0) {
        LOG_ERROR("tracker add timer failed!\n");
        return -1;
    }

    return 0;
}

static int
tracker_timer_del(struct tracker *tr)
{
    if(tr->tmrfd < 0) {
        return -1;
    }

    if(timer_del(tr->tsk->epfd, tr->tmrfd)) {
        LOG_ERROR("tracker del timer failed!\n");
        return -1;
    }

    close(tr->tmrfd);
    tr->tmrfd = -1;

    return 0;
}

static int
tracker_connect(struct torrent_task *tsk)
{
    struct tracker *tr = &tsk->tr;
    struct torrent_file *tor = &tsk->tor;

    int i = tr->url_index;
    for(; i < tor->tracker_num; i++) {

        reset_tracker_members(tr);

        if(http_url_parser(tor->tracker_url[i], &tr->tp) == -1) {
            continue;
        }

        if(tr->tp.prot_type == TRACKER_PROT_UDP) {
            continue;
        }

        if(get_ip_address_info(&tr->tp, &tr->ai) == -1) {
            continue;
        }

        if((tr->sockid = create_tracker_client_socket(&tr->tp, tr->ai)) < 0) {
            continue;
        }

        int errNo = 0;
        if(connect_tracker_server(tr->sockid, &tr->tp, tr->ai, &errNo) == -1) {
            continue;
        }

        tr->url_index = i;

        tr->state = TRACKER_STATE_SENDING_REQ;
        if(errNo && tr->tp.prot_type != TRACKER_PROT_UDP) {
            tr->state = TRACKER_STATE_CONNECTING;
        }

        return 0;
    }

    return -1;
}

static int
tracker_add_event(int event, struct tracker *tr)
{
    struct event_param ep;
    ep.fd = tr->sockid;
    ep.event = event;
    ep.evt_ctx = tr;
    ep.evt_hdl = (event_handle_t)tracker_event_handle;

    if(event_add(tr->tsk->epfd, &ep)) {
        LOG_ERROR("tracker add event failed!\n");
        return -1;
    }

    return 0;
}

static int
tracker_mod_event(int event, struct tracker *tr)
{
    struct event_param ep;
    ep.fd = tr->sockid;
    ep.event = event; 
    ep.evt_ctx = tr;
    ep.evt_hdl = (event_handle_t)tracker_event_handle;

    if(event_mod(tr->tsk->epfd, &ep)) {
        LOG_ERROR("tracker mod event failed!\n");
        return -1;
    }

    return 0;
}

static int
tracker_del_event(struct tracker *tr)
{
    struct event_param ep;

    memset(&ep, 0, sizeof(ep));
    ep.fd = tr->sockid;

    if(event_del(tr->tsk->epfd, &ep)) {
        LOG_ERROR("tracker del event failed!\n");
    }

    close(tr->sockid);
    tr->sockid = -1;

    return 0;
}

static int
tracker_try_announce(struct torrent_task *tsk)
{
    tsk->tr.url_index++;
    return tracker_announce(tsk);
}

int
tracker_announce(struct torrent_task *tsk)
{
    struct tracker *tr;
    
    tr = &tsk->tr;
    tr->tsk = tsk;

    if(tracker_connect(tsk)) {
        LOG_INFO("tracker_connect failed!\n");
        return -1;
    }

    if(tracker_add_event(EPOLLIN | EPOLLOUT, tr)) {
        LOG_ERROR("tracker(%s:%s)add event failed!\n", tr->tp.host, tr->tp.prot_type);
        reset_tracker_members(tr);
        return -1;
    }

    if(tr->state == TRACKER_STATE_CONNECTING) {
        if(tracker_timer_add(tr)) {
            LOG_ERROR("tracker(%s:%s)start timer failed!\n", tr->tp.host, tr->tp.prot_type);
        }
    }

    return 0;
}

static int
tracker_parser_bencode(struct tracker *tr, struct benc_type *bt)
{
    int interval;
    if(handle_int_kv(bt, "interval", &interval)) {
        LOG_ERROR("no found interval key!\n");
        return -1;
    }

    int buflen;
    char *peers;
    if(handle_string_kv(bt, "peers", &peers, &buflen)) {
        LOG_ERROR("no found peers key!\n");
        return -1;
    }

    if(!buflen || (buflen % 6 != 0)) {
        LOG_ERROR("peers num[%d] invalid!\n", buflen);
        return -1;
    }

    int complete = 0;
    handle_int_kv(bt, "complete", &complete);

    int incomplete = 0;
    handle_int_kv(bt, "incomplete", &incomplete);

    LOG_DEBUG("interval:%d, complete:%d, incomplete:%d\n", interval, complete, incomplete);
    
    int i, npeer = buflen / 6;
    for(i = 0; i < npeer; i++) {
        char *peer = peers + i*6, peeraddr[32]; 

        utils_strf_addrinfo(*(int *)peer, *(unsigned short *)(peer+4), peeraddr, 32);
        LOG_DEBUG("peer[%s]\n", peeraddr);

        if(tr->npeer < MAX_PEER_NUM && !peer_init(tr, peer)) {
            tr->npeer++;
        }
    }

    return 0;
}

static int
tracker_parser_response(struct tracker *tr, char *rspbuf, int buflen)
{
    if(!rspbuf || buflen <= 0) {
        return -1;
    }

    struct offset offsz;
    offsz.begin = rspbuf;
    offsz.end = rspbuf + buflen;
     
    struct benc_type bt;
    memset(&bt, 0, sizeof(bt));
    if(parser_dict(&offsz, &bt)) {
        return -1;
    }

    struct benc_type *fail;
    if((fail = get_dict_value_by_key(&bt, "failure reason", BENC_TYPE_STRING))) {
        LOG_DEBUG("tracker response failed:%s\n", fail->val.str.s);
        // destroy_dict(&bt);
        return -1;
    }

    if(tracker_parser_bencode(tr, &bt)) {
        LOG_DUMP(rspbuf, buflen, "tracker parser failed:");
        // destroy_dict(&bt);
        return -1;
    }

    // destroy_dict(&bt);

    return 0;
}

static int
tracker_event_handle_connecting(int event, struct tracker *tr)
{
    struct torrent_task *tsk;

    tsk = tr->tsk;

    if(event & EPOLLOUT) {

        tracker_timer_del(tr);

        int errNo = 0;
        if(get_socket_opt(tr->sockid, SO_ERROR, &errNo)) {
            LOG_ERROR("get_socket_opt failed:%s\n", strerror(errno));
            return -1;
        }

        if(errNo) {
            LOG_INFO("connect (%s:%s): %s\n", tr->tp.host, tr->tp.port, strerror(errNo));
            tracker_del_event(tr);
            tr->state = TRACKER_STATE_NONE;
            if(++tr->url_index < tsk->tor.tracker_num) {
                LOG_INFO("try another announce!\n");
                return tracker_try_announce(tsk);
            } else {
                LOG_INFO("try all announce and failed!\n");
            }
            return -1;
        }

        LOG_INFO("connect (%s:%s) ok.\n", tr->tp.host, tr->tp.port);

        tr->state = TRACKER_STATE_SENDING_REQ;
        tracker_mod_event(EPOLLIN|EPOLLOUT, tr);

        return 0;
    }

    LOG_ERROR("no expected event[%d] in connecting state!\n", event);

    return -1;
}

static int
tracker_event_handle_sendreq(int event, struct tracker *tr)
{
    if(event & EPOLLIN) {
        LOG_ALARM("recv read event in %d state!\n", tr->state);
    }

    if(event & EPOLLOUT) {
        if(http_request(tr)) {
            LOG_DEBUG("send request (%s:%s)failed!\n", tr->tp.host, tr->tp.port);
            tracker_del_event(tr);
            tr->state = TRACKER_STATE_NONE;
            if(tracker_try_announce(tr->tsk)) {
                LOG_INFO("connect tracker for torrent[%s]\n", tr->tsk->tor.torfile);
            }
            return 0;
        }
        LOG_DEBUG("send request (%s:%s)OK!\n", tr->tp.host, tr->tp.port);
        tr->state = TRACKER_STATE_WAITING_RSP;
        tracker_mod_event(EPOLLIN, tr);
        tracker_timer_add(tr);
    }

    return 0;
}

static int
tracker_event_handle_waitrsp(int event, struct tracker *tr)
{
    if(event & EPOLLOUT) {
        LOG_ALARM("recv write event in %d state!\n", tr->state);
    }

    if(event & EPOLLIN) {

        struct http_rsp_buf rspbuf;
        memset(&rspbuf, 0, sizeof(rspbuf));

        int res = http_response(tr, &rspbuf);

        tracker_timer_del(tr);
        tracker_del_event(tr);

        if(res) {
            LOG_DEBUG("recv response(%s:%s)failed!\n", tr->tp.host, tr->tp.port);
            reset_tracker_members(tr);
            if(tracker_try_announce(tr->tsk)) {
                LOG_INFO("can't connect tracker for torrent[%s]\n", tr->tsk->tor.torfile);
            }
        } else {
            LOG_DEBUG("recv response(%s:%s)OK!\n", tr->tp.host, tr->tp.port);
            if(tracker_parser_response(tr, rspbuf.body, rspbuf.bodysz)) {
                reset_tracker_members(tr);
                if(tracker_try_announce(tr->tsk)) {
                    LOG_INFO("can't connect tracker for torrent[%s]\n", tr->tsk->tor.torfile);
                }
            }
        }

        free(rspbuf.rcvbuf);
#if 0
        reset_tracker_members(tr);
        if(tracker_try_announce(tr->tsk)) {
            LOG_INFO("can't connect tracker for torrent[%s]\n", tr->tsk->tor.torfile);
        }
#endif

    }

    return 0;
}

static int
tracker_event_handle(int event, void *evt)
{
    struct tracker *tr;

    tr = (struct tracker *)evt;
    switch(tr->state) {
        case TRACKER_STATE_CONNECTING:
            return tracker_event_handle_connecting(event, tr);
        case TRACKER_STATE_SENDING_REQ:
            return tracker_event_handle_sendreq(event, tr);
        case TRACKER_STATE_WAITING_RSP:
            return tracker_event_handle_waitrsp(event, tr);
        default:
            LOG_ALARM("invalid state[%d] when event[%x] occur!\n", tr->state, event);
            break;
    }
    return -1;
}

