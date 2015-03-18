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
#include "event.h"
#include "tracker.h"
#include "timer.h"
#include "log.h"
#include "torrent.h"
#include "peer.h"
#include "utils.h"
#include "tortask.h"
#include "mempool.h"

#define INIT_CONN_ID 0x41727101980

static int tracker_udp_socket_init(struct tracker *tr);
static int tracker_udp_connect(struct tracker *tr);
static int tracker_udp_send_connect_req(struct tracker *tr);
static int tracker_udp_send_announce_req(struct tracker *tr);
static int tracker_udp_connect_req(struct tracker *tr);
static int tracker_udp_connect_rsp(struct tracker *tr);
static int tracker_udp_announce_req(struct tracker *tr);
static int tracker_udp_announce_rsp(struct tracker *tr);

static int tracker_udp_timeout_handle(int event, void *evt);
static int tracker_udp_event_handle(int event, void *evt);

static int
tracker_udp_socket_init(struct tracker *tr)
{
    tr->sockid = socket_udp_create();
    if(tr->sockid < 0) {
        return -1;
    }

    if(set_socket_unblock(tr->sockid)) {
        close(tr->sockid);
        tr->sockid = -1;
        return -1;
    }

    if(socket_udp_connect(tr->sockid, tr->ip, tr->port)) {
        close(tr->sockid);
        tr->sockid = -1;
        return -1;
    }

    return 0;
}

static int
tracker_udp_connect(struct tracker *tr)
{
	if(!tr->ip && get_ip_address_info(&tr->tp, &tr->ip, &tr->port) == -1) {
		return -1;
	}

	if(tracker_udp_socket_init(tr)) {
		return -1;
	}

    LOG_DEBUG("[%s:%s] connect ok!\n", tr->tp.host, tr->tp.port);

	return 0;
}

/* connid+action+transactionid */
static int
tracker_udp_send_connect_req(struct tracker *tr)
{
    char req_msg[16];
    memset(req_msg, 0, sizeof(req_msg));

    int64 connid = socket_hton64(INIT_CONN_ID);
    memcpy(req_msg, &connid, 8);

    memcpy(req_msg+12, &tr->transaction_id, 4);

    if(socket_udp_send(tr->sockid, req_msg, sizeof(req_msg), 0) != sizeof(req_msg)) {
        LOG_ERROR("tracker[%s:%s] send udp: [%s]!\n", tr->tp.host, tr->tp.port, strerror(errno));
        return -1;
    }

    LOG_DEBUG("[%s:%s] connect request.\n", tr->tp.host, tr->tp.port);

    return 0; 
}

static int
tracker_udp_send_announce_req(struct tracker *tr)
{
    struct torrent_task *tsk = tr->tsk;

    char req_msg[98];
    memset(req_msg, 0, sizeof(req_msg));

    memcpy(req_msg, &tr->conn_id, 8);

    int action = socket_htonl(1);
    memcpy(req_msg+8, &action, 4);

    memcpy(req_msg+12, &tr->transaction_id, 4);

    memcpy(req_msg+16, tsk->tor.info_hash, SHA1_LEN);
    memcpy(req_msg+36, peer_id, PEER_ID_LEN);

    uint64 downsz = socket_hton64(0);
    memcpy(req_msg+56, &downsz, 8);

    uint64 leftsz = socket_hton64((int64)tsk->leftpieces * tsk->tor.piece_len);
    memcpy(req_msg+64, &leftsz, 8);

    uint64 uploadsz = socket_hton64(0);
    memcpy(req_msg+72, &uploadsz, 8);

    int event = !tr->announce_cnt ? 2 : 0;
    event = socket_htonl(event);
    memcpy(req_msg+80, &event, 4);

    /*  skip ip 4byte */
   /* skip key 4byte */ 

    int numwant = socket_htonl(-1);
    memcpy(req_msg+92, &numwant, 4);

    uint16 port = socket_htons(tsk->listen_port);
    memcpy(req_msg+96, &port, 2);

    if(socket_udp_send(tr->sockid, req_msg, sizeof(req_msg), 0) != sizeof(req_msg)) {
        LOG_ERROR("tracker[%s:%s] send failed: [%s]!\n", tr->tp.host, tr->tp.port, strerror(errno));
        return -1;
    }

    LOG_DEBUG("[%s:%s] annouce request.\n", tr->tp.host, tr->tp.port);

    return 0;
}

static int
tracker_udp_connect_req(struct tracker *tr)
{
    tr->transaction_id = socket_htonl(time(NULL));

    if(tracker_udp_send_connect_req(tr)) {
        goto FAILED;
    }

    tr->state = TRACKER_STATE_UDP_CONNECT_RSP;
    if(tracker_mod_event(EPOLLIN, tr, tracker_udp_event_handle)) {
        LOG_ERROR("[%s:%s] modify event failed!\n", tr->tp.host, tr->tp.port);
        goto FAILED;
    }

    if(tracker_start_timer(tr, 1500)) {
        LOG_ERROR("[%s:%s] start timer failed!\n", tr->tp.host, tr->tp.port);
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
tracker_udp_connect_rsp(struct tracker *tr)
{
    tracker_stop_timer(tr);

    char rsp_msg[64];

    int rcvlen = socket_udp_recv(tr->sockid, rsp_msg, sizeof(rsp_msg), 0);
    if(rcvlen < 16) {
        LOG_ERROR("[%s:%s] connect rsp failed[%d][%s]!\n",
                tr->tp.host, tr->tp.port, rcvlen, strerror(errno));
        goto FAILED;
    }

    int action, transaction_id;
    memcpy(&action, rsp_msg, 4);
    memcpy(&transaction_id, rsp_msg+4, 4);

    if(action != 0 || transaction_id != tr->transaction_id) {
        LOG_ERROR("[%s:%s] connect rsp failed!\n", tr->tp.host, tr->tp.port);
        goto FAILED;
    }

    tr->state = TRACKER_STATE_UDP_ANNOUNCE_REQ;
    if(tracker_mod_event(EPOLLOUT, tr, tracker_udp_event_handle)) {
        LOG_ERROR("[%s:%s] modify event failed\n", tr->tp.host, tr->tp.port);
        goto FAILED;
    }

    memcpy(&tr->conn_id, rsp_msg+8, 8);

    LOG_ERROR("[%s:%s] connect id[%llx]\n", tr->tp.host, tr->tp.port, socket_ntoh64(tr->conn_id));

    return 0;

FAILED:
    tracker_destroy_timer(tr);
    tracker_del_event(tr);
    tracker_reset_members(tr);
    torrent_tracker_recycle(tr->tsk, tr, 0);
    return -1;
}

static int
tracker_udp_announce_req(struct tracker *tr)
{
    tr->connect_cnt = 0;

    tr->transaction_id = socket_htonl(time(NULL));

    if(tracker_udp_send_announce_req(tr)) {
        goto FAILED;
    }

    tr->state = TRACKER_STATE_UDP_ANNOUNCE_RSP;
    if(tracker_mod_event(EPOLLIN, tr, tracker_udp_event_handle)) {
        LOG_ERROR("[%s:%s] modify event failed\n", tr->tp.host, tr->tp.port);
        goto FAILED;
    }

    if(tracker_start_timer(tr, 1500)) {
        LOG_ERROR("[%s:%s] start timer failed\n", tr->tp.host, tr->tp.port);
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
tracker_udp_announce_rsp(struct tracker *tr)
{
    tracker_stop_timer(tr);

    int active = 0;

    char rsp_msg[1024];

    int rcvlen = socket_udp_recv(tr->sockid, rsp_msg, sizeof(rsp_msg), 0);
    if(rcvlen < 16) {
        LOG_ERROR("[%s:%s] recv failed\n", tr->tp.host, tr->tp.port);
        goto FREE;
    }

    int action, transaction_id;
    memcpy(&action, rsp_msg, 4);
    action = socket_ntohl(action);
    memcpy(&transaction_id, rsp_msg+4, 4);

    /* maybe recv retransmit connect rsp, just ignore */
    if(action == 0) {
        tracker_start_timer(tr, 1500);
        return 0;
    }

    if(rcvlen < 20 || action != 1 || transaction_id != tr->transaction_id) {
        LOG_ERROR("[%s:%s] connect rsp failed!\n", tr->tp.host, tr->tp.port);
        goto FREE;
    }

    int interval;
    memcpy(&interval, rsp_msg+8, 4);
    interval = socket_ntohl(interval);

    int leecher;
    memcpy(&leecher, rsp_msg+12, 4);
    leecher = socket_ntohl(leecher);

    int seeder;
    memcpy(&seeder, rsp_msg+16, 4);
    seeder = socket_ntohl(seeder);

    LOG_DEBUG("[%s:%s] interval[%d] leecher[%d], seeder[%d]!\n",
                tr->tp.host, tr->tp.port, interval, leecher, seeder);

    int addrslen = rcvlen - 20;
    if(addrslen % 6 != 0) {
        LOG_ERROR("[%s:%s] peer addrslen[%d] invalid!\n", tr->tp.host, tr->tp.port, addrslen);
        goto FREE;
    }

    int i, npeer = addrslen / 6;
    for(i = 0; i < npeer; i++) {
        char *peer = &rsp_msg[20+i*6], peeraddr[32];

        utils_strf_addrinfo(*(int *)peer, *(unsigned short *)(peer+4), peeraddr, 32);
        LOG_DEBUG("peer[%s]\n", peeraddr);

        torrent_add_peer_addrinfo(tr->tsk, peer);
    }

    tr->annouce_time = time(NULL) + interval;
    tr->announce_cnt++;
    active = 1;

FREE:
    tracker_destroy_timer(tr);
    tracker_del_event(tr);
    tracker_reset_members(tr);
    torrent_tracker_recycle(tr->tsk, tr, active);
    return 0;
}

static int
tracker_udp_event_handle(int event, void *evt)
{
    struct tracker *tr;

    tr = (struct tracker *)evt;
    switch(tr->state) {
        case TRACKER_STATE_UDP_CONNECT_REQ:
            return tracker_udp_connect_req(tr);
        case TRACKER_STATE_UDP_CONNECT_RSP:
            return tracker_udp_connect_rsp(tr);
        case TRACKER_STATE_UDP_ANNOUNCE_REQ:
            return tracker_udp_announce_req(tr);
        case TRACKER_STATE_UDP_ANNOUNCE_RSP:
            return tracker_udp_announce_rsp(tr);
        default:
            LOG_ALARM("invalid state[%d] when event[%x] occur!\n", tr->state, event);
            break;
    }
    return -1;
}

static int
tracker_udp_timeout_handle(int event, void *evt_ctx)
{
    struct tracker *tr = (struct tracker *) evt_ctx;

    int64 tmrbuf;
    if(read(tr->tmrfd, &tmrbuf, sizeof(tmrbuf)) != sizeof(tmrbuf)) {
        LOG_ALARM("[%s:%s] read timer fd failed\n", tr->tp.host, tr->tp.port);
    }

    if(++tr->connect_cnt > 4) {
        LOG_INFO("(%s:%s) timeout\n", tr->tp.host, tr->tp.port);
        goto FAILED;
    }

    switch(tr->state) {
        case TRACKER_STATE_UDP_CONNECT_RSP:
            if(tracker_udp_send_connect_req(tr)) {
                goto FAILED;
            }
            tracker_start_timer(tr, 1500);
            return 0;
        case TRACKER_STATE_UDP_ANNOUNCE_RSP:
            if(tracker_udp_send_announce_req(tr)) {
                goto FAILED;
            }
            tracker_start_timer(tr, 1500);
            return 0;
        default:
            LOG_ALARM("invalid state[%d] when event[%x] occur!\n", tr->state, event);
            break;
    }

FAILED:
    tracker_destroy_timer(tr);
    tracker_del_event(tr);
    tracker_reset_members(tr);
    torrent_tracker_recycle(tr->tsk, tr, 0);
    return 0;
}

int
tracker_udp_announce(struct tracker *tr)
{
	tracker_reset_members(tr);

    int active = 0;
    tr->state = TRACKER_STATE_UDP_CONNECT_REQ;

    if(tracker_udp_connect(tr)) {
        LOG_INFO("tracker(%s:%s) connect failed!\n", tr->tp.host, tr->tp.port);
        active = 2;
		goto FAILED;
    }

    if(tracker_add_event(EPOLLOUT, tr, tracker_udp_event_handle)) {
        LOG_ERROR("tracker(%s:%s)add event failed!\n", tr->tp.host, tr->tp.port);
		goto FAILED;
    }

    if(tracker_create_timer(tr, tracker_udp_timeout_handle)) {
        LOG_ERROR("tracker(%s:%s)create timer failed!\n", tr->tp.host, tr->tp.port);
        tracker_del_event(tr);
        goto FAILED;
    }
 
    return 0;

FAILED:
	tracker_reset_members(tr);
	torrent_tracker_recycle(tr->tsk, tr, active);
	return -1;
}

