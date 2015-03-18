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
#include "tracker.h"
#include "log.h"
#include "torrent.h"
#include "peer.h"
#include "utils.h"
#include "tortask.h"
#include "mempool.h"

static int tracker_event_handle_connecting(int event, struct tracker *tr);
static int tracker_event_handle_sendreq(int event, struct tracker *tr);
static int tracker_event_handle_waitrsp(int event, struct tracker *tr);
static int tracker_timeout_handle(int event, void *evt_ctx);
static int tracker_event_handle(int event, void *evt);
static int tracker_parser_response(struct tracker *tr, char *rspbuf, int buflen);
static int tracker_connect(struct tracker *tr);

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
	tracker_reset_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, 0);

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

    LOG_DEBUG("interval:%d, complete:%d, incomplete:%d, sendme[%d]\n",
                                            interval, complete, incomplete, buflen/6);
    
    int i, npeer = buflen / 6;
    for(i = 0; i < npeer; i++) {
        char *peer = peers + i*6, peeraddr[32]; 

        utils_strf_addrinfo(*(int *)peer, *(unsigned short *)(peer+4), peeraddr, 32);
        LOG_DEBUG("peer[%s]\n", peeraddr);

		torrent_add_peer_addrinfo(tr->tsk, peer);
    }

	tr->annouce_time = time(NULL) + interval;

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
        destroy_dict(&bt);
        return -1;
    }

    if(tracker_parser_bencode(tr, &bt)) {
        destroy_dict(&bt);
        return -1;
    }

     destroy_dict(&bt);

    return 0;
}

static int
tracker_event_handle_connecting(int event, struct tracker *tr)
{
    tracker_stop_timer(tr);

    int errNo = 0;
    if(get_socket_opt(tr->sockid, SO_ERROR, &errNo)) {
        LOG_ERROR("get_socket_opt failed:%s\n", strerror(errno));
        goto FAILED;
    }

    if(errNo) {
        LOG_INFO("(%s:%s)connect: %s\n", tr->tp.host, tr->tp.port, strerror(errNo));
        goto FAILED;
    }

    LOG_INFO("(%s:%s)connect ok.\n", tr->tp.host, tr->tp.port);
    tr->state = TRACKER_STATE_SENDING_REQ;

    return 0;

FAILED:
    tracker_destroy_timer(tr);
    tracker_del_event(tr);
	tracker_reset_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, 0);
	return -1;
}

static int
tracker_event_handle_sendreq(int event, struct tracker *tr)
{
    tracker_stop_timer(tr);

    if(http_request(tr)) {
        LOG_DEBUG("send request (%s:%s)failed!\n", tr->tp.host, tr->tp.port);
        goto FAILED;
    }

    LOG_DEBUG("send request (%s:%s)ok!\n", tr->tp.host, tr->tp.port);

    tr->state = TRACKER_STATE_WAITING_RSP;

    if(tracker_mod_event(EPOLLIN, tr, tracker_event_handle)) {
        goto FAILED;
    }

    if(tracker_start_timer(tr, 800)) {
        goto FAILED;
    }

    return 0;

FAILED:
    tracker_destroy_timer(tr);
    tracker_del_event(tr);
	tracker_reset_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, 0);
	return -1;
}

static int
tracker_event_handle_waitrsp(int event, struct tracker *tr)
{
    int res, active = 0;

    struct http_rsp_buf rspbuf;
    memset(&rspbuf, 0, sizeof(rspbuf));

    if((res = http_response(tr, &rspbuf))) {
        LOG_DEBUG("recv response(%s:%s)failed!\n", tr->tp.host, tr->tp.port);
        goto FREE;
    }

    if(tracker_parser_response(tr, rspbuf.body, rspbuf.bodysz)) {
        LOG_DEBUG("response parser (%s:%s)failed!\n", tr->tp.host, tr->tp.port);
        goto FREE;
    }

    LOG_DEBUG("response(%s:%s)ok!\n", tr->tp.host, tr->tp.port);
    tr->announce_cnt++;
    active = 1;

FREE:
    GFREE(rspbuf.rcvbuf);
    tracker_destroy_timer(tr);
    tracker_del_event(tr);
	tracker_reset_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, active);
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

int
tracker_http_announce(struct tracker *tr)
{
	tracker_reset_members(tr);

    int active = 0;

    if(tracker_connect(tr)) {
        LOG_INFO("tracker(%s:%s) connect failed!\n", tr->tp.host, tr->tp.port);
        active = 2;
		goto FAILED;
    }

    if(tracker_add_event(EPOLLOUT, tr, tracker_event_handle)) {
        LOG_ERROR("tracker(%s:%s)add event failed!\n", tr->tp.host, tr->tp.port);
		goto FAILED;
    }

    if(tracker_create_timer(tr, tracker_timeout_handle)) {
        LOG_ERROR("tracker(%s:%s)create timer failed!\n", tr->tp.host, tr->tp.port);
        tracker_del_event(tr);
        goto FAILED;
    }

    if(tr->state == TRACKER_STATE_CONNECTING) {
        if(tracker_start_timer(tr, 800)) {
            LOG_ERROR("tracker(%s:%s)start timer failed!\n", tr->tp.host, tr->tp.prot_type);
            tracker_destroy_timer(tr);
			tracker_del_event(tr);
            goto FAILED;
        }
    }

    return 0;

FAILED:
	tracker_reset_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, active);
	return -1;
}

