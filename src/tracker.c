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
#include <time.h>
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
#include "tortask.h"

static int reset_tracker_members(struct tracker *tr);

static int tracker_add_event(int event, struct tracker *tr);
static int tracker_mod_event(int event, struct tracker *tr);
static int tracker_del_event(struct tracker *tr);

static int tracker_destroy_timer(struct tracker *tr);
static int tracker_stop_timer(struct tracker *tr);
static int tracker_start_timer(struct tracker *tr);
static int tracker_create_timer(struct tracker *tr);

static int tracker_event_handle_connecting(int event, struct tracker *tr);
static int tracker_event_handle_sendreq(int event, struct tracker *tr);
static int tracker_event_handle_waitrsp(int event, struct tracker *tr);

static int tracker_timeout_handle(int event, void *evt_ctx);
static int tracker_event_handle(int event, void *evt);

static int tracker_parser_response(struct tracker *tr, char *rspbuf, int buflen);
static int tracker_connect(struct tracker *tr);

static int
reset_tracker_members(struct tracker *tr)
{
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
tracker_timeout_handle(int event, void *evt_ctx)
{
    struct tracker *tr = (struct tracker *) evt_ctx;

    LOG_INFO("handle (%s:%s) timeout\n", tr->tp.host, tr->tp.port);

    long long tmrbuf;
    if(read(tr->tmrfd, &tmrbuf, sizeof(tmrbuf)) != sizeof(tmrbuf)) {
        LOG_ALARM("read timer fd failed\n");
    }

    tracker_destroy_timer(tr);
    tracker_del_event(tr);
	reset_tracker_members(tr);

	torrent_tracker_recycle(tr->tsk, tr, 0);

    return 0;
}

static int
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

static int
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

static int
tracker_start_timer(struct tracker *tr)
{
    if(tr->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.tmrfd = tr->tmrfd;
    tp.time = 800;
    tp.interval = 0;

    if(timer_start(&tp)) {
        LOG_ERROR("tracker start timer failed!\n");
        return -1;
    }

    return 0;
}

static int
tracker_create_timer(struct tracker *tr)
{
    if(tr->tmrfd >= 0) {
        LOG_ALARM("tracker tmrfd is [%d] when create timer!\n", tr->tmrfd);
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.epfd = tr->tsk->epfd;
    tp.tmr_hdl = tracker_timeout_handle;
    tp.tmr_ctx = tr;

    if(timer_creat(&tp)) {
        LOG_ERROR("tracker create timer failed!\n");
        return -1;
    }

    tr->tmrfd = tp.tmrfd;

    return 0;
}

static int
tracker_socket_init(struct tracker *tr)
{
    tr->sockid = socket_tcp_create();
    if(tr->sockid < 0) {
        return -1;
    }

    if(set_socket_unblock(tr->sockid)) {
        close(tr->sockid);
        tr->sockid = -1;
        return -1;
    }

    int res = socket_tcp_connect(tr->sockid, tr->ip, tr->port);
    if(!res) {
        LOG_DEBUG("%s:%s connecting...ok!\n", tr->tp.host, tr->tp.port);
		tr->state = TRACKER_STATE_SENDING_REQ;
    } else if(errno == EINPROGRESS) {
        LOG_DEBUG("%s:%s connecting...in progress!\n", tr->tp.host, tr->tp.port);
		tr->state = TRACKER_STATE_CONNECTING;
    } else {
        close(tr->sockid);
        tr->sockid = -1;
        return -1;
    }

    return 0;
}

static int
tracker_connect(struct tracker *tr)
{
	reset_tracker_members(tr);

	if(tr->tp.prot_type == TRACKER_PROT_UDP) {
		LOG_ERROR("tracker not support udp[%s:%s] protocol currently!\n", tr->tp.host, tr->tp.port);
		return -1;
	}

	if(!tr->ip && get_ip_address_info(&tr->tp, &tr->ip, &tr->port) == -1) {
		return -1;
	}

	if(tracker_socket_init(tr)) {
		return -1;
	}

	return 0;
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

	tr->annouce_time = time(NULL) + interval;

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

    LOG_DEBUG("interval:%d, complete:%d, incomplete:%d, sendme[%d]\n",
                                            interval, complete, incomplete, buflen/6);
    
    int i, npeer = buflen / 6;
    for(i = 0; i < npeer; i++) {
        char *peer = peers + i*6, peeraddr[32]; 

        utils_strf_addrinfo(*(int *)peer, *(unsigned short *)(peer+4), peeraddr, 32);
        LOG_DEBUG("peer[%s]\n", peeraddr);

		torrent_add_peer_addrinfo(tr->tsk, peer);
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
        // destroy_dict(&bt);
        return -1;
    }

    // destroy_dict(&bt);

    return 0;
}

static int
tracker_event_handle_connecting(int event, struct tracker *tr)
{
    if(event & EPOLLOUT) {

        tracker_stop_timer(tr);

        int errNo = 0;
        if(get_socket_opt(tr->sockid, SO_ERROR, &errNo)) {
            LOG_ERROR("get_socket_opt failed:%s\n", strerror(errno));
			goto FAILED;
        }

        if(errNo) {
            LOG_INFO("(%s:%s)connect: %s\n", tr->tp.host, tr->tp.port, strerror(errNo));
            tr->state = TRACKER_STATE_NONE;
			goto FAILED;
        }

        LOG_INFO("(%s:%s)connect ok.\n", tr->tp.host, tr->tp.port);

        tr->state = TRACKER_STATE_SENDING_REQ;
        tracker_mod_event(EPOLLIN|EPOLLOUT, tr);

        return 0;
    }

    LOG_ERROR("no expected event[%d] in connecting state!\n", event);

FAILED:
    tracker_destroy_timer(tr);
    tracker_del_event(tr);
	reset_tracker_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, 0);
	return -1;
}

static int
tracker_event_handle_sendreq(int event, struct tracker *tr)
{
    if(event & EPOLLIN) {
        LOG_ALARM("recv read event in %d state!\n", tr->state);
    }

    tracker_stop_timer(tr);

    if(event & EPOLLOUT) {
        if(http_request(tr)) {
            LOG_DEBUG("send request (%s:%s)failed!\n", tr->tp.host, tr->tp.port);
            tr->state = TRACKER_STATE_NONE;
			goto FAILED;
        }
        LOG_DEBUG("send request (%s:%s)ok!\n", tr->tp.host, tr->tp.port);
        tr->state = TRACKER_STATE_WAITING_RSP;
        tracker_mod_event(EPOLLIN, tr);
    }

    tracker_start_timer(tr);

    return 0;

FAILED:
    tracker_destroy_timer(tr);
    tracker_del_event(tr);
	reset_tracker_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, 0);
	return -1;
}

static int
tracker_event_handle_waitrsp(int event, struct tracker *tr)
{
    if(event & EPOLLOUT) {
        LOG_ALARM("recv write event in %d state!\n", tr->state);
    }

    tracker_destroy_timer(tr);
    tracker_del_event(tr);

    if(event & EPOLLIN) {

        struct http_rsp_buf rspbuf;
        memset(&rspbuf, 0, sizeof(rspbuf));

        int res = http_response(tr, &rspbuf);

        if(res) {
            LOG_DEBUG("recv response(%s:%s)failed!\n", tr->tp.host, tr->tp.port);
        	free(rspbuf.rcvbuf);
			goto FAILED;
        } else {
            LOG_DEBUG("recv response(%s:%s)OK!\n", tr->tp.host, tr->tp.port);
            if(tracker_parser_response(tr, rspbuf.body, rspbuf.bodysz)) {
        		free(rspbuf.rcvbuf);
				goto FAILED;
            } else {
        		free(rspbuf.rcvbuf);
			}
        }
    }

    tr->announce_cnt++;
	reset_tracker_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, 1);
    return 0;

FAILED:
	reset_tracker_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, 0);
	return -1;
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

int
tracker_announce(struct tracker *tr)
{
    tr->sockid = -1;
    tr->tmrfd = -1;

    int active = 0;

    if(tracker_connect(tr)) {
        LOG_INFO("tracker(%s:%s) connect failed!\n", tr->tp.host, tr->tp.port);
        active = 2;
		goto FAILED;
    }

    if(tracker_add_event(EPOLLIN | EPOLLOUT, tr)) {
        LOG_ERROR("tracker(%s:%s)add event failed!\n", tr->tp.host, tr->tp.port);
		goto FAILED;
    }

    if(tracker_create_timer(tr)) {
        LOG_ERROR("tracker(%s:%s)create timer failed!\n", tr->tp.host, tr->tp.port);
        tracker_del_event(tr);
        goto FAILED;
    }

    if(tr->state == TRACKER_STATE_CONNECTING) {
        if(tracker_start_timer(tr)) {
            LOG_ERROR("tracker(%s:%s)start timer failed!\n", tr->tp.host, tr->tp.prot_type);
            tracker_destroy_timer(tr);
			tracker_del_event(tr);
            goto FAILED;
        }
    }

    return 0;

FAILED:
	reset_tracker_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, active);
	return -1;
}

