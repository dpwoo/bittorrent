#include <sys/epoll.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include "btype.h"
#include "peer.h"
#include "timer.h"
#include "log.h"
#include "socket.h"
#include "event.h"
#include "bitfield.h"
#include "torrent.h"
#include "tortask.h"
#include "utils.h"
#include "mempool.h"

#define MAX_BUFFER_LEN (1024*8)

extern char peer_id[];

static int peer_reset_member(struct peer *pr);

static int peer_socket_init(struct peer *pr);

static int peer_check_piece_sha1(struct peer *pr);
static int peer_down_complete_slice(struct peer *pr);

static int peer_start(struct peer *pr);
static int peer_connecting_timeout(struct peer *pr);
static int peer_handshake_timeout(struct peer *pr);
static int peer_connected_timeout(struct peer *pr);
static int peer_exchange_bitfield_timeout(struct peer *pr);

static int peer_add_event(struct peer *pr, int event);
static int peer_mod_event(struct peer *pr, int event);
static int peer_del_event(struct peer *pr);

static int peer_create_timer(struct peer *pr);
static int peer_stop_timer(struct peer *pr);
static int peer_destroy_timer(struct peer *pr);
static int peer_start_timer(struct peer *pr);

static int peer_send_data(struct peer *pr, char *data, int len);
static int peer_recv_data(struct peer *pr, char *rcvbuf, int buflen);

static int peer_send_handshake_msg(struct peer *pr);
static int peer_recv_handshake_msg(struct peer *pr);
static int peer_send_bitfiled_msg(struct peer *pr);
static int peer_recv_bitfield_msg(struct peer *pr);
static int peer_send_keepalive_msg(struct peer *pr);

static int peer_send_slice_header(struct peer *pr, struct slice *sl);
static int peer_send_slice_data(struct peer *pr);

static int peer_send_chocked_msg(struct peer *pr);
static int peer_send_unchocked_msg(struct peer *pr);
static int peer_send_intrested_msg(struct peer *pr);
static int peer_send_notintrested_msg(struct peer *pr);
static int peer_send_have_msg(struct peer *pr);
static int peer_send_request_msg(struct peer *pr, struct peer_rcv_msg *pm);
static int peer_send_cancel_msg(struct peer *pr, int idx, int offset, int sz);

static int peer_recv_choked_msg(struct peer *pr);
static int peer_recv_unchoked_msg(struct peer *pr);
static int peer_recv_intrested_msg(struct peer *pr);
static int peer_recv_notintrested_msg(struct peer *pr);
static int peer_recv_have_msg(struct peer *pr);
static int peer_recv_request_msg(struct peer *pr);
static int peer_recv_cancel_msg(struct peer *pr);
static int peer_recv_keepalive_msg(struct peer *pr);
static int peer_recv_piece_msg(struct peer *pr);

static int peer_parser_msg(struct peer *pr, struct peer_rcv_msg *pm);

static int peer_timeout_handle(int evnet, void *evt_ctx);
static int peer_event_handle(int event, void *evt_ctx);

static int peer_event_connecting(struct peer *pr, int event);
static int peer_event_handshake(struct peer *pr, int event);
static int peer_event_connected(struct peer *pr, int event);

static int 
peer_reset_member(struct peer *pr)
{
    peer_destroy_timer(pr);
    peer_del_event(pr);

/* bitmap */
    GFREE(pr->bf.bitmap);
    pr->bf.bitmap = NULL;

    pr->pm.data_transfering = 0;
    pr->pm.rcvlen = 0;
    if(pr->pm.rcvbuf) {
        GFREE(pr->pm.rcvbuf);
        pr->pm.rcvbuf = NULL;
    }

    if(pr->having_pieces) {
        struct pieces *p;
        while(pr->having_pieces) {
            p = pr->having_pieces;
            pr->having_pieces = p->next;
            GFREE(p);
        } 
        pr->having_pieces = NULL;
    }

/* data downloading list */
    if(pr->pm.req_list || pr->pm.downed_list || pr->pm.wait_list) {
        *pr->pm.req_tail = pr->pm.wait_list;
        *pr->pm.downed_tail = pr->pm.req_list;
        pr->pm.wait_list = pr->pm.downed_list;

        struct slice *tmp = pr->pm.wait_list;
        while(tmp) {
            GFREE(tmp->data);
            tmp->data = NULL;
            tmp = tmp->next;
        }

        int idx = pr->pm.wait_list->idx;

        if(pr->pm.wait_list->offset == 0) {
            GFREE(pr->pm.wait_list);
        } else {
            LOG_ERROR("peer[%s] not complete slice, maybe memory leak!\n");
        }

        pr->pm.wait_list = NULL;

        pr->pm.downed_list = NULL;
        pr->pm.downed_tail = &pr->pm.downed_list;

        pr->pm.req_list = NULL;
        pr->pm.req_tail = &pr->pm.req_list;

        bitfield_peer_giveup_piece(&pr->tsk->bf, idx);
    }

/* data uploading list */
    GFREE(pr->psm.piecedata);
    pr->psm.piecedata = NULL;

    struct slice *tmp, *sl;
    if(pr->psm.req_list) {
       for(sl = pr->psm.req_list; sl;) {
            tmp = sl;
            sl = sl->next;
            GFREE(tmp);
       }
    }

    pr->psm.req_list = NULL;
    pr->psm.req_tail = &pr->psm.req_list;

    return 0;
}

static int
peer_socket_init(struct peer *pr)
{
    pr->sockid = socket_tcp_create();
    if(pr->sockid < 0) {
        return -1;
    }

    if(set_socket_unblock(pr->sockid)) {
        close(pr->sockid);
        pr->sockid = -1;
        return -1;
    }

    int res = socket_tcp_connect(pr->sockid, pr->ipaddr->ip, pr->ipaddr->port);
    if(!res) {
        pr->state = PEER_STATE_SEND_HANDSHAKE;
    } else if(errno == EINPROGRESS) {
        pr->state = PEER_STATE_CONNECTING;
    } else {
        close(pr->sockid);
        pr->sockid = -1;
        return -1;
    }

    return 0;
}

static int
peer_down_complete_slice(struct peer *pr)
{
    struct peer_rcv_msg *pm = &pr->pm;

    struct slice *tmp = pm->req_list;
    pm->req_list = pm->req_list->next; 
    if(!pm->req_list) {
        pm->req_tail = &pm->req_list;
    }

    *pm->downed_tail = tmp;
    tmp->next = NULL;
    pm->downed_tail = &tmp->next;

    if(!pm->wait_list && !pm->req_list) {
        if(!pm->downed_list || pm->downed_list->offset != 0) {
            LOG_ERROR("peer[%s] download error occur, and memory may leak!\n", pr->strfaddr);
            return -1;
        }
        peer_check_piece_sha1(pr);
        GFREE(pm->downed_list);
        pm->downed_list = NULL;
        pm->downed_tail = &pm->downed_list;
    }

    return 0;
}

static int
peer_check_piece_sha1(struct peer *pr)
{
    struct peer_rcv_msg *pm = &pr->pm;

    char *buffer = GMALLOC(pr->bf.piecesz);
    if(!buffer) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    int offset = 0;
    int idx = pm->downed_list->idx;

    struct slice *sl = pm->downed_list;
    for(; sl; sl = sl->next) {
        memcpy(buffer + offset, sl->data, sl->slicesz);
        offset += sl->slicesz;
        GFREE(sl->data);
    }

    if(utils_sha1_check(buffer, offset, &pr->tsk->tor.pieces[idx * 20], 20)) {
        LOG_ERROR("peer[%s] piece[%d] sha1 check failed!\n", pr->strfaddr, idx);
        bitfield_peer_giveup_piece(&pr->tsk->bf, idx);
        GFREE(buffer);
        return -1;
    }

    if(torrent_write_piece(pr->tsk, idx, buffer, offset)) {
        LOG_ERROR("peer[%s] write piece[%d]failed!\n", pr->strfaddr, idx);
        GFREE(buffer);
        return -1;
    }

    if(bitfield_local_have(&pr->tsk->bf, idx)) {
        GFREE(buffer);
        return -1;
    }

    torrent_add_having_piece(pr->tsk, idx);

    pr->tsk->leftpieces--;
    LOG_DEBUG("peer[%s] piece[%d] complete!\n", pr->strfaddr, idx);

    GFREE(buffer);
    return 0;
}

static int
peer_connecting_timeout(struct peer *pr)
{
    LOG_INFO("peer[%s] connecting timeout!\n", pr->strfaddr);

    peer_reset_member(pr);

    return 0;
}

static int
peer_handshake_timeout(struct peer *pr)
{
    LOG_INFO("peer[%s] handshake timeout!\n", pr->strfaddr);

    peer_reset_member(pr);

    return 0;
}

static int
peer_exchange_bitfield_timeout(struct peer *pr)
{
    LOG_INFO("peer[%s] exchange bitfield timeout!\n", pr->strfaddr);

    peer_reset_member(pr);

    return 0;
}

static int
peer_connected_timeout(struct peer *pr)
{
    peer_start_timer(pr);

    if(pr->having_pieces) {
        peer_send_have_msg(pr);
        return 0;
    }

    if(pr->heartbeat < time(NULL)) {
        peer_send_keepalive_msg(pr);
        pr->heartbeat = time(NULL) + 60;
    }

    return 0;
}

static int
peer_start(struct peer *pr)
{
    if(!pr->ipaddr->client && peer_socket_init(pr)) {
        LOG_ERROR("peer[%s] socket init failed.\n", pr->strfaddr);
        peer_destroy_timer(pr);
        pr->isused = 0;
        return -1;
    }

    if(pr->state == PEER_STATE_SEND_HANDSHAKE) {
        LOG_INFO("peer[%s] connecting...OK\n", pr->strfaddr);

        if(peer_send_handshake_msg(pr)) {
            LOG_ERROR("send peer[%s] handshake msg failed!\n", pr->strfaddr);
            goto FAILED;
        }

        if(peer_add_event(pr, EPOLLIN)) {
            LOG_ERROR("peer[%s] add event failed!\n", pr->strfaddr);
            goto FAILED;
        }

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", pr->strfaddr);
            peer_del_event(pr);
            goto FAILED;
        }

        return 0;
    } else { /* PEER_STATE_CONNECTING */
        LOG_INFO("peer[%s] connecting...in progress\n", pr->strfaddr);

        if(peer_add_event(pr, EPOLLOUT)) {
            LOG_ERROR("peer[%s] add event failed!\n", pr->strfaddr);
            goto FAILED;
        }

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", pr->strfaddr);
            peer_del_event(pr);
            goto FAILED;
        }

        return 0;
    }

FAILED:
    peer_destroy_timer(pr);
    if(pr->sockid > 0) {
        close(pr->sockid);
        pr->sockid = -1;
    }
    pr->isused = 0;
    return -1;
}

static int
peer_timeout_handle(int event, void *evt_ctx)
{
    struct peer *pr;
    pr = (struct peer *)evt_ctx;

    long long tmrbuf;
    if(read(pr->tmrfd, &tmrbuf, sizeof(tmrbuf)) != sizeof(tmrbuf)) {
        LOG_ALARM("timer read tmrbuf failed:%s\n", strerror(errno));
    }

    peer_stop_timer(pr);
    
    switch(pr->state) {
        case PEER_STATE_NONE:
            if(peer_start(pr)) {
                torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NONE);
                break;
            }
            return 0;
        case PEER_STATE_CONNECTING:
            peer_connecting_timeout(pr);
            torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NONE);
            break;
        case PEER_STATE_SEND_HANDSHAKE:
            peer_handshake_timeout(pr);
            torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NONE);
            break;
        case PEER_STATE_EXCHANGE_BITFIELD:
            peer_exchange_bitfield_timeout(pr);
            torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NONE);
            break;
        case PEER_STATE_CONNECTD:
            return peer_connected_timeout(pr);       
        default:
            LOG_ERROR("timeout occur in unexpect state[%d]!\n", pr->state);
            break;
    }

    return -1;
}

static int
peer_compute_time(struct peer *pr)
{
    switch(pr->state) {
        case PEER_STATE_NONE:
            return 100;
        case PEER_STATE_CONNECTING:
            return 600;
        case PEER_STATE_SEND_HANDSHAKE:
            return 600;
        case PEER_STATE_EXCHANGE_BITFIELD:
            return 600;
        case PEER_STATE_CONNECTD:
        {
            int time = pr->having_pieces ? 10 : (pr->psm.req_list ? 5: 12000);
            return time;
        }
        default:
            LOG_ERROR("unexpect peer state[%d]\n", pr->state);
    }

    return 0;
}

static int
peer_destroy_timer(struct peer *pr)
{
    if(pr->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.epfd = pr->tsk->epfd;
    tp.tmrfd = pr->tmrfd;
    
    if(timer_destroy(&tp)) {
        LOG_ERROR("peer destroy timer failed!\n");
        return -1;
    }

    close(pr->tmrfd);
    pr->tmrfd = -1;

    return 0;
}

static int
peer_stop_timer(struct peer *pr)
{
    if(pr->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.tmrfd = pr->tmrfd;
    
    if(timer_stop(&tp)) {
        LOG_ERROR("peer stop timer failed!\n");
        return -1;
    }

    return 0;
}

static int
peer_start_timer(struct peer *pr)
{
    if(pr->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.tmrfd = pr->tmrfd;
    tp.time = peer_compute_time(pr);
    tp.interval = 0;

    if(timer_start(&tp)) {
        LOG_ERROR("peer start timer failed!\n");
        return -1;
    }

    return 0;
}

static int
peer_create_timer(struct peer *pr)
{
    if(pr->tmrfd > 0) {
        LOG_ALARM("peer tmrfd is [%d] when create timer!\n", pr->tmrfd);
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.epfd = pr->tsk->epfd;
    tp.tmr_hdl = peer_timeout_handle;
    tp.tmr_ctx = pr;

    if(timer_creat(&tp)) {
        LOG_ERROR("peer create timer failed!\n");
        return -1;
    }

    pr->tmrfd = tp.tmrfd;
 
    return 0;
}

/* we need quick timeout to do something like sending having msg etc.*/
int
peer_modify_timer_time(struct peer *pr, int time)
{
    if(pr->state != PEER_STATE_CONNECTD || pr->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.tmrfd = pr->tmrfd;
    tp.time = time; 
    tp.interval = 0;

    if(timer_start(&tp)) {
        LOG_ERROR("peer start timer failed!\n");
        return -1;
    }

    return 0;
}

static int
peer_add_event(struct peer *pr, int event)
{
    struct event_param ep;
    ep.event = event;
    ep.fd = pr->sockid;
    ep.evt_hdl = peer_event_handle;
    ep.evt_ctx = pr;

    if(event_add(pr->tsk->epfd, &ep)) {
        LOG_ERROR("peer add event failed!\n");
        return -1;
    }

    return 0;
}

static int
peer_mod_event(struct peer *pr, int event)
{
    struct event_param ep;
    ep.event = event;
    ep.fd = pr->sockid;
    ep.evt_hdl = peer_event_handle;
    ep.evt_ctx = pr;

    if(event_mod(pr->tsk->epfd, &ep)) {
        LOG_ERROR("peer mod event failed!\n");
        return -1;
    }

    return 0;
}

static int
peer_del_event(struct peer *pr)
{
    if(pr->sockid < 0) {
        return -1;
    }

    struct event_param ep;
    memset(&ep, 0, sizeof(ep));
    ep.fd = pr->sockid;

    if(event_del(pr->tsk->epfd, &ep)) {
        LOG_ERROR("peer del event failed!\n");
        return -1;
    }

    close(pr->sockid);
    pr->sockid = -1;

    return 0;
}

/* return value:
 * 0--> msg ok;
 * -1--> error msg format, should close sockid;
 * -2-->should wait enough byte for a msg.
 * */
static int
peer_parser_msg(struct peer *pr, struct peer_rcv_msg *pm)
{
    if(pm->rcvlen < 4) {
        return -2;
    }

    int len_pre;
    memcpy(&len_pre, pm->rcvbuf, 4);
    len_pre = socket_ntohl(len_pre);

    if(!len_pre) {
        return peer_recv_keepalive_msg(pr);
    }

    if(pm->rcvlen == 4) {
        return -2;
    }

    switch(pm->rcvbuf[4]) {
        case PEER_MSG_ID_CHOCKED:
            if(len_pre == 1) {
                return peer_recv_choked_msg(pr);
            }
            break;
        case PEER_MSG_ID_UNCHOCKED:
            if(len_pre == 1) {
                return peer_recv_unchoked_msg(pr);
            }
            break;
        case PEER_MSG_ID_INSTRESTED:
            if(len_pre == 1) {
                return peer_recv_intrested_msg(pr);
            }
            break;
        case PEER_MSG_ID_NOTINSTRESTED:
            if(len_pre == 1) {
                return peer_recv_notintrested_msg(pr);
            }
            break;
        case PEER_MSG_ID_HAVE:
            if(len_pre == 5 && pm->rcvlen >= 9) {
                return peer_recv_have_msg(pr);
            } else if(len_pre == 5 && pm->rcvlen < 9) {
                return -2;
            }
            break;
        case PEER_MSG_ID_BITFIELD:
            if(len_pre == 1+pr->tsk->bf.nbyte && pm->rcvlen >= 5+pr->tsk->bf.nbyte) {
                return peer_recv_bitfield_msg(pr);    
            } else if(len_pre == 1+pr->tsk->bf.nbyte && pm->rcvlen < 5+pr->tsk->bf.nbyte) {
                return -2;
            }
            break;
        case PEER_MSG_ID_REQUEST:
            if(len_pre == 13 && pm->rcvlen >= 17) {
                return peer_recv_request_msg(pr);
            } else if(len_pre == 13 && pm->rcvlen < 17) {
                return -2;
            }
            break;
        case PEER_MSG_ID_CANCEL:
            if(len_pre == 13 && pm->rcvlen >= 17) {
                return peer_recv_cancel_msg(pr);
            } else if(len_pre == 13 && pm->rcvlen < 17) {
                return -2;
            }
            break;
        case PEER_MSG_ID_PIECE:
            if(len_pre > 9 && pm->rcvlen >= 13) {
                return peer_recv_piece_msg(pr);
            } else if(len_pre > 9 && pm->rcvlen < 13) {
                return -2;
            }
            break;
        case PEER_MSG_ID_PORT:
            if(len_pre == 3 && pm->rcvlen >= 7) {
                pm->rcvlen -= 7;
                if(pm->rcvlen) {
                    memmove(pm->rcvbuf, pm->rcvbuf+7, pm->rcvlen);
                }
                return 0;
            } else if(len_pre == 3 && pm->rcvlen < 7) {
                return -2;
            }
            break;
        default:
            LOG_ERROR("peer[%s] recv unexpected msgtype[%d]\n", pr->strfaddr, pm->rcvbuf[4]);
    }

    return -1;
}

static int
peer_send_data(struct peer *pr, char *data, int len)
{
    pr->heartbeat = time(NULL) + 60;
    return socket_tcp_send_all(pr->sockid, data, len);
}

static int
peer_recv_data(struct peer *pr, char *rcvbuf, int buflen)
{
    return socket_tcp_recv(pr->sockid, rcvbuf, buflen, 0);
}

static int
peer_send_slice_header(struct peer *pr, struct slice *sl)
{
    char msg[13] = {0, 0, 0, 0, PEER_MSG_ID_PIECE, };

    int len_pre = socket_htonl(9+sl->slicesz);
    memcpy(msg, &len_pre, 4);

    int idx = socket_htonl(sl->idx);
    memcpy(msg+5, &idx, 4);

    int offset = socket_htonl(sl->offset);
    memcpy(msg+9, &offset, 4);

    if(peer_send_data(pr, msg, sizeof(msg))) {
        LOG_DEBUG("peer[%s] send piece msg hdr failed\n", pr->strfaddr);
        return -1;
    }

    LOG_DEBUG("peer[%s] send piece msg hdr[%d,%d] \n", pr->strfaddr, sl->idx, sl->offset);

    return 0;
}

static int
peer_send_slice_data(struct peer *pr)
{
    if(!pr->psm.req_list) {
        return 0;
    }

    struct slice *sl = pr->psm.req_list;

    if(!pr->psm.piecedata || pr->psm.pieceidx != sl->idx) {
        GFREE(pr->psm.piecedata);
        pr->psm.piecedata = NULL;
        if(torrent_read_piece(pr->tsk, sl->idx, &pr->psm.piecedata, &pr->psm.piecesz)) {
            LOG_ERROR("peer[%s] torrent read piece[%d] failed\n", pr->strfaddr, sl->idx);
            return -1;
        }
        pr->psm.pieceidx = sl->idx;
    }

    if(sl->offset + sl->slicesz > pr->psm.piecesz) {
        LOG_ERROR("peer[%s] invalid slice[%d,%d,%d]\n", pr->strfaddr,
                                        sl->offset, sl->slicesz, pr->psm.piecesz);
        return -1;
    }

    if(!pr->psm.sliceoffset && peer_send_slice_header(pr, sl)) {
        return -1;
    }

    int offset = sl->offset+pr->psm.sliceoffset;
    int size = sl->slicesz < MTU_SZ ? sl->slicesz : MTU_SZ; 

    sl->slicesz -= size;
    pr->psm.sliceoffset += size;

    if(peer_send_data(pr, pr->psm.piecedata+offset, size)) {
        LOG_DEBUG("peer[%s] send data[%d,%d,%d] failed\n",
                                    pr->strfaddr, sl->idx, offset, size);
        return -1;
    }

#if 0
    LOG_DEBUG("peer[%s] send data[%d,%d,%d]\n", pr->strfaddr, sl->idx, offset, size);
#endif

    if(sl->slicesz > 0) {
        return 0;
    }

    pr->psm.sliceoffset = 0;
    pr->psm.req_list = sl->next;
    if(!pr->psm.req_list) {
        pr->psm.req_tail = &pr->psm.req_list;
    }
    GFREE(sl);

    return 0;
}

static int
peer_send_chocked_msg(struct peer *pr)
{
    LOG_INFO("peer[%s] send chocked msg\n", pr->strfaddr);

    pr->am_unchoking = 0;

    char msg[5] = {0, 0, 0, 1, PEER_MSG_ID_CHOCKED};
    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }
    return 0;
}

static int
peer_send_unchocked_msg(struct peer *pr)
{
    LOG_INFO("peer[%s] send unchocked msg\n", pr->strfaddr);

    char msg[5] = {0, 0, 0, 1, PEER_MSG_ID_UNCHOCKED};
    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }
    return 0;
}

static int
peer_send_intrested_msg(struct peer *pr)
{
    LOG_INFO("peer[%s] send intrested msg\n", pr->strfaddr);

    char msg[5] = {0, 0, 0, 1, PEER_MSG_ID_INSTRESTED};
    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }
    pr->am_interested = 1;
    return 0;
}

static int
peer_send_notintrested_msg(struct peer *pr)
{
    LOG_INFO("peer[%s] send notintrested msg\n", pr->strfaddr);

    char msg[5] = {0, 0, 0, 1, PEER_MSG_ID_NOTINSTRESTED};
    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }
    return 0;
}

static int
peer_send_have_msg(struct peer *pr)
{
    struct pieces *tmp, **p = &pr->having_pieces;

    char msg[9] = {0, 0, 0, 5, PEER_MSG_ID_HAVE};

    while(*p) {
        LOG_INFO("peer[%s] send have msg[%d]\n", pr->strfaddr, (*p)->idx);

        int idx = socket_htonl((*p)->idx);
        memcpy(msg+5, &idx, 4);

        if(peer_send_data(pr, msg, sizeof(msg))) {
            return -1;
        }
        tmp = *p;
        *p = tmp->next;
        GFREE(tmp);
    }

    return 0;
}

static int
peer_send_request_msg(struct peer *pr, struct peer_rcv_msg *pm)
{
    if(!pm->req_list && !pm->wait_list) {
       if(bitfield_get_request_piece(&pr->tsk->bf, &pr->bf, pm)) {
            LOG_DEBUG("peer[%s] have no piece for us!\n", pr->strfaddr);
            return -1;
        }
    }

    int curreq = 0, maxreq = 4;
    struct slice *tmp = pm->req_list;
    for(; tmp; tmp = tmp->next) {
        curreq++;
    }

    struct slice **sl;
    for(sl = &pm->wait_list; *sl && curreq < maxreq; curreq++) {

        LOG_DEBUG("peer[%s] send request msg[%d,%d,%d]\n",
                  pr->strfaddr, (*sl)->idx, (*sl)->offset, (*sl)->slicesz);

        char msg[17] = {0, 0, 0, 13, PEER_MSG_ID_REQUEST};

        int idx = socket_htonl((*sl)->idx);
        memcpy(msg+5, &idx, 4);

        int offset = socket_htonl((*sl)->offset);
        memcpy(msg+9, &offset, 4);

        int sz = socket_htonl((*sl)->slicesz);
        memcpy(msg+13, &sz, 4);

        if(peer_send_data(pr, msg, sizeof(msg))) {
            LOG_ERROR("peer[%s] send request msg[%d,%d,%d] failed\n",
                  pr->strfaddr, (*sl)->idx, (*sl)->offset, (*sl)->slicesz);
            return -1;
        }

        tmp = *sl;
        *sl = (*sl)->next;

        *pm->req_tail = tmp;
        pm->req_tail = &tmp->next;
        *pm->req_tail = NULL;
    }

    return 0;
}

static int
peer_send_cancel_msg(struct peer *pr, int idx, int offset, int sz)
{
    char msg[17] = {0, 0, 0, 13, PEER_MSG_ID_CANCEL};

    idx = socket_htonl(idx);
    memcpy(msg+5, &idx, 4);

    offset = socket_htonl(offset);
    memcpy(msg+9, &offset, 4);

    sz = socket_htonl(sz);
    memcpy(msg+13, &sz, 4);

    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }

    return 0;
}

static int
peer_send_keepalive_msg(struct peer *pr)
{
    char buf[4];
    memset(buf, 0, sizeof(buf));

    if(peer_send_data(pr, buf, sizeof(buf))) {
        LOG_ERROR("peer[%s] send keepalive failed:%s\n",pr->strfaddr, strerror(errno));
        return -1;
    }

    LOG_INFO("peer[%s] send keepalive msg ok!\n", pr->strfaddr);

    return 0;
}

static int
peer_send_handshake_msg(struct peer *pr)
{
    char handshake[68], *s = handshake;
    s += snprintf(handshake, sizeof(handshake), "%c%s", 19, "BitTorrent protocol");

    char reserved[8];
    memset(reserved, 0, sizeof(reserved));
    memcpy(s, reserved, sizeof(reserved));
    s += sizeof(reserved);

    memcpy(s, pr->tsk->tor.info_hash, SHA1_LEN);
    s += SHA1_LEN;
    memcpy(s, peer_id, PEER_ID_LEN);

    if(peer_send_data(pr, handshake, sizeof(handshake))) {
        LOG_ERROR("peer[%s] send handshake msg: %s\n", pr->strfaddr, strerror(errno));
        return -1;
    }

    return 0;
}

/* 19+bt_proto_str+peerid */
static int
peer_recv_handshake_msg(struct peer *pr)
{
    char handshake[68]; 

    int rcvlen = socket_tcp_recv(pr->sockid, handshake, sizeof(handshake), 0);
    if(rcvlen != sizeof(handshake)) {
        LOG_ERROR("peer[%s] recv handshake failed[%d:%s]!\n",
                            pr->strfaddr, rcvlen, strerror(errno));
        return -1;
    }

    if(handshake[0] != (unsigned char)19
                    || memcmp(handshake+1, "BitTorrent protocol", 19)
                    || memcmp(handshake+28, pr->tsk->tor.info_hash, SHA1_LEN)) {
        LOG_ERROR("peer[%s] recv handshake content invalid!\n", pr->strfaddr);
        return -1;
    }

    memcpy(pr->peerid, handshake+48, PEER_ID_LEN);
    if(!memcmp(peer_id, pr->peerid, PEER_ID_LEN)) {
        LOG_ERROR("peer[%s] we connect ourself!\n", pr->strfaddr);
        return -1;
    }

    LOG_DEBUG("peer[%s] handshake ok, client peerid:%.8s\n", pr->strfaddr, pr->peerid);

    return 0;
}

static int
peer_send_bitfiled_msg(struct peer *pr)
{
    struct bitfield *bf;
    bf = &pr->tsk->bf;

    char msghdr[5] = {0, 0, 0, 0, PEER_MSG_ID_BITFIELD};
    int msglen = socket_htonl(1+bf->nbyte);
    memcpy(msghdr, &msglen, 4);

    struct iovec iovs[2];
    iovs[0].iov_base = msghdr;
    iovs[0].iov_len = 5;
    iovs[1].iov_base = bf->bitmap; 
    iovs[1].iov_len = bf->nbyte; 

    int wlen = socket_tcp_send_iovs(pr->sockid, iovs, 2);
    if(wlen < 0) {
        LOG_ERROR("peer[%s] send bitfield failed:%s\n", pr->strfaddr, strerror(errno));
        return -1;
    } else if(wlen != 5+bf->nbyte) {
        if(wlen < 5) {
            if(peer_send_data(pr, msghdr+wlen, 5-wlen)) {
                return -1;
            }
            wlen = 5;
        }
        if(peer_send_data(pr, bf->bitmap, bf->nbyte+5 - wlen)) {
            return -1;
        }
    }

    LOG_INFO("peer[%s] send bitfield ok!\n", pr->strfaddr);

    return 0;
}

/* len_pre+id+bitmap */
static int
peer_recv_bitfield_msg(struct peer *pr)
{
    if(pr->bf.bitmap) {
        LOG_ERROR("peer[%s] bitmap exist when recv bitfield msg!\n", pr->strfaddr);
        return -1;
    }

    int rcvlen = pr->pm.rcvlen;
    char *rcvbuf = pr->pm.rcvbuf;

    int len_pre;
    memcpy(&len_pre, rcvbuf, 4);
    len_pre = socket_ntohl(len_pre);

    if(rcvbuf[4] != PEER_MSG_ID_BITFIELD || len_pre-1 != pr->tsk->bf.nbyte) {
        LOG_ERROR("peer[%s] read bitfield invalid\n", pr->strfaddr);
        return -1;
    }

    if(bitfield_create(&pr->bf, pr->tsk->bf.npieces, pr->tsk->bf.piecesz, pr->tsk->bf.totalsz)) {
        LOG_ERROR("peer[%s] bitfield create failed!\n", pr->strfaddr);
        return -1;
    }

    if(bitfield_dup(&pr->bf, rcvbuf+5, len_pre-1)) {
        LOG_ERROR("peer[%s] bitfield dup failed!\n", pr->strfaddr);
        return -1;
    }

    LOG_DUMP(pr->bf.bitmap, pr->bf.nbyte, "peer[%s]bitfield[%d]:",
            pr->strfaddr, pr->bf.nbyte);

    if(bitfield_intrested(&pr->tsk->bf, &pr->bf)) {
        if(peer_send_intrested_msg(pr)) {
            LOG_ERROR("peer[%s] send intrested msg failed!\n", pr->strfaddr);
            return -1;
        }
    }

    pr->pm.rcvlen = rcvlen-4-len_pre;
    if(pr->pm.rcvlen) {
        memmove(rcvbuf, rcvbuf+4+len_pre, pr->pm.rcvlen);
    }

    return 0;
}

/* len_pre+id */
static int
peer_recv_choked_msg(struct peer *pr)
{
    LOG_INFO("peer[%s] recv chocked msg!\n", pr->strfaddr);
    pr->peer_unchoking = 0;
    peer_send_intrested_msg(pr);

    struct peer_rcv_msg *pm = &pr->pm;
    if(pm->req_list) {
        *pm->req_tail = pm->wait_list;
        pm->wait_list = pm->req_list;
        pm->req_list = NULL;
        pm->req_tail = &pm->req_list;
        struct slice *tmp = pm->wait_list;
        while(tmp) {
            GFREE(tmp->data);
            tmp->data = NULL;
            tmp = tmp->next;
        }
    }

    pm->rcvlen -= 5;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+5, pm->rcvlen);
    }

    return 0;
}

/* len_pre+id */
static int
peer_recv_unchoked_msg(struct peer *pr)
{
    LOG_INFO("peer[%s] recv unchocked msg!\n", pr->strfaddr);
    pr->peer_unchoking = 1;

    peer_send_request_msg(pr, &pr->pm);

    pr->pm.rcvlen -= 5;
    if(pr->pm.rcvlen) {
        memmove(pr->pm.rcvbuf, pr->pm.rcvbuf+5, pr->pm.rcvlen);
    }
 
    return 0;
}

/* len_pre+id */
static int
peer_recv_intrested_msg(struct peer *pr)
{
    LOG_INFO("peer[%s] recv intrested msg!\n", pr->strfaddr);
    pr->peer_interested = 1;
    pr->am_unchoking = 1;
    peer_send_unchocked_msg(pr);

    struct peer_rcv_msg *pm;
    pm = &pr->pm;
    pm->rcvlen -= 5;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+5, pm->rcvlen);
    }

    /* peer not have any piece, and not sending bitfield to us */
    if(!pr->bf.bitmap) {
        if(bitfield_create(&pr->bf, pr->tsk->bf.npieces,
                           pr->tsk->bf.piecesz, pr->tsk->bf.totalsz)) {
            LOG_ERROR("peer[%s] bitfield create failed!\n", pr->strfaddr);
            return -1;
        }
    }

    return 0;
}

/* len_pre+id */
static int
peer_recv_notintrested_msg(struct peer *pr)
{
    LOG_INFO("peer[%s] recv notintrested msg!\n", pr->strfaddr);
    pr->peer_interested = 0;
    pr->am_unchoking = 0;

    struct peer_rcv_msg *pm;
    pm = &pr->pm;
    pm->rcvlen -= 5;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+5, pm->rcvlen);
    }

    return 0;
}

/* len_pre+id+idx */
static int
peer_recv_have_msg(struct peer *pr)
{
    struct peer_rcv_msg *pm;
    pm = &pr->pm;

    int idx;
    memcpy(&idx, pm->rcvbuf+5, 4);
    idx = socket_ntohl(idx);

    pm->rcvlen -= 9;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+9, pm->rcvlen);
    }

    LOG_INFO("peer[%s] recv have msg[%d]!\n", pr->strfaddr, idx);

    if(!bitfield_peer_have(&pr->tsk->bf, &pr->bf, idx) && !pr->am_interested) {
        peer_send_intrested_msg(pr);
    }

    return 0;
}

/* len_pre+id+idx+offset+size */
static int
peer_recv_request_msg(struct peer *pr)
{
    struct peer_rcv_msg *pm;
    pm = &pr->pm;

    int idx, offset, size;

    memcpy(&idx, pm->rcvbuf+5, 4);
    idx = socket_ntohl(idx);

    memcpy(&offset, pm->rcvbuf+9, 4);
    offset = socket_ntohl(offset);

    memcpy(&size, pm->rcvbuf+13, 4);
    size = socket_ntohl(size);

    pm->rcvlen -= 17;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+17, pm->rcvlen);
    }

    LOG_INFO("peer[%s] recv request msg[%d,%d,%d]!\n", pr->strfaddr, idx, offset, size);

    if(!pr->am_unchoking) {
        LOG_INFO("peer[%s] request error[%d,%d,%d]!\n", pr->strfaddr, idx, offset, size);
        peer_send_chocked_msg(pr);
        return 0;
    }

    if(bitfield_is_local_have(&pr->tsk->bf, idx, offset, size)) {
        LOG_INFO("peer[%s] request error[%d,%d,%d]!\n", pr->strfaddr, idx, offset, size);
        peer_send_chocked_msg(pr);
        return 0;
    }

    if(!pr->psm.req_list) {
        /* peer_modify_timer_time(pr, 10); */
        peer_mod_event(pr, EPOLLIN | EPOLLOUT);
    }

    struct slice *req;
    if(!(req = GCALLOC(1, sizeof(*req)))) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }
    
    req->idx = idx;
    req->offset = offset;
    req->slicesz = size;

    *pr->psm.req_tail = req;
    pr->psm.req_tail = &req->next;

    return 0;
}

/* len_pre+id+idx+offset+size */
static int
peer_recv_cancel_msg(struct peer *pr)
{
    struct peer_rcv_msg *pm;
    pm = &pr->pm;

    int idx, offset, size;

    memcpy(&idx, pm->rcvbuf+5, 4);
    idx = socket_ntohl(idx);

    memcpy(&offset, pm->rcvbuf+9, 4);
    offset = socket_ntohl(offset);

    memcpy(&size, pm->rcvbuf+13, 4);
    size = socket_ntohl(size);

    pm->rcvlen -= 17;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+17, pm->rcvlen);
    }

    LOG_INFO("peer[%s] recv cancel msg[%d,%d,%d]!\n", pr->strfaddr, idx, offset, size);

return 0;

    struct slice **sl;
    for(sl = &pr->psm.req_list; *sl; sl = &(*sl)->next) {
        if((*sl)->idx == idx && (*sl)->offset == offset) {
            break;
        }
    }

    if(*sl) {
        if(*sl == pr->psm.req_list) {
            pr->psm.sliceoffset = 0;
        }

        struct slice *tmp = *sl;
        *sl = (*sl)->next;
        free(tmp);

        if(!pr->psm.req_list) {
            pr->psm.req_tail = &pr->psm.req_list;
        }
    }

    return 0;
}

/* len_pre=0000 */
static int
peer_recv_keepalive_msg(struct peer *pr)
{
    LOG_DEBUG("peer[%s] recv keepalive msg!\n", pr->strfaddr);

    struct peer_rcv_msg *pm;
    pm = &pr->pm;
    pm->rcvlen -= 4;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+4, pm->rcvlen);
    }

    return 0;
}

static int
peer_recv_left_piece_msg(struct peer *pr)
{
    struct peer_rcv_msg *pm = &pr->pm;
    if(!pm->req_list || !pm->req_list->data) {
        LOG_ERROR("peer[%s] download error!\n", pr->strfaddr);
        return -1;
    }

    int totalsz = pm->req_list->downsz + pm->rcvlen;

    pr->ipaddr->downsz += pm->rcvlen;

    if(totalsz >= pm->req_list->slicesz) {
        int len = totalsz - pm->req_list->slicesz;

        memcpy(pm->req_list->data + pm->req_list->downsz, pm->rcvbuf, pm->rcvlen - len);

        if(len) {
            memmove(pr->pm.rcvbuf, pr->pm.rcvbuf+pr->pm.rcvlen-len, len);
            pr->pm.rcvlen = len;
        } else {
            pr->pm.rcvlen = 0;
        }

        pm->data_transfering = 0;

#if 1
        LOG_DEBUG("peer[%s] recv piece msg[%d, %d, %d]\n",
                pr->strfaddr, pm->req_list->idx, pm->req_list->offset, pm->req_list->slicesz);
#endif
        if(peer_down_complete_slice(pr)) {
            return -1;
        }

        if(peer_send_request_msg(pr, pm)) {
            return -1;
        }

    } else {
        memcpy(pm->req_list->data + pm->req_list->downsz, pm->rcvbuf, pm->rcvlen);
        pm->req_list->downsz += pm->rcvlen;
        pr->pm.rcvlen = 0;
    }

    return 0;
}

/* piece msg : len_pre+id+idx+offset+data */
static int
peer_recv_piece_msg(struct peer *pr)
{
    struct peer_rcv_msg *pm;
    pm = &pr->pm;

    int idx, offset;
    memcpy(&idx, pm->rcvbuf+5, 4);
    idx = socket_ntohl(idx);

    memcpy(&offset, pm->rcvbuf+9, 4);
    offset = socket_ntohl(offset);

    char *data = pm->rcvbuf + 13;
    int datasz = pm->rcvlen - 13;

    int len_pre;
    memcpy(&len_pre, pm->rcvbuf, 4);
    len_pre = socket_ntohl(len_pre);

    if(pm->req_list == NULL) {
        LOG_ERROR("peer[%s] download error[%d,%d,%d]!\n", pr->strfaddr, idx, offset, datasz);
        return -1;
    }

    if(len_pre - 9 != pm->req_list->slicesz
                   || pm->req_list->offset != offset || pm->req_list->idx != idx) {
        LOG_ERROR("peer[%s] recv not correct piece msg[%d,%d]!\n", pr->strfaddr, idx, offset);
        return -1;
    }

    pr->ipaddr->downsz += datasz;

    pm->req_list->data = GMALLOC(pm->req_list->slicesz);
    if(!pm->req_list->data) {
        LOG_ERROR("out of memory[%d]\n", pm->req_list->slicesz);
        return -1;
    }

    if(datasz < pm->req_list->slicesz) { /* part of slice */
        memcpy(pm->req_list->data, data, datasz);
        pm->req_list->downsz = datasz;
        pm->data_transfering = 1;
        pm->rcvlen = 0;
    } else { /* a small slice data */
        memcpy(pm->req_list->data, data, pm->req_list->slicesz);

        pm->rcvlen = datasz - pm->req_list->slicesz;
        if(pm->rcvlen) {
            memmove(pm->rcvbuf, pm->rcvbuf+13+pm->req_list->slicesz, pm->rcvlen);     
        }

        peer_down_complete_slice(pr);

        peer_send_request_msg(pr, pm);
    }

    return 0;
}

static int
peer_event_connecting(struct peer *pr, int event)
{
    if(event & EPOLLIN) {
        /* LOG_ALARM("read event occur in connecting state!\n"); */
    }

    if(event & EPOLLOUT) {
        
        peer_stop_timer(pr);

        int errNo = 0;
        if(get_socket_error(pr->sockid, &errNo)) {
            LOG_ERROR("get_socket_error failed:%s\n", strerror(errno));
            goto FAILED;
        }

        if(errNo) {
            LOG_INFO("peer[%s] connect: %s\n", pr->strfaddr, strerror(errNo));
            goto FAILED;
        }

        LOG_INFO("peer[%s] connect ok.\n", pr->strfaddr);

        if(peer_send_handshake_msg(pr)) {
            LOG_ERROR("peer[%s] send handshake msg failed!\n", pr->strfaddr);
            goto FAILED;
        }

        if(peer_mod_event(pr, EPOLLIN)) {
            LOG_ERROR("peer[%s] mod event failed!\n", pr->strfaddr);
            goto FAILED;
        }

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", pr->strfaddr);
            goto FAILED;
        }

        LOG_INFO("peer[%s] send handshake msg ok!\n", pr->strfaddr);

        pr->state = PEER_STATE_SEND_HANDSHAKE;

        return 0;
    }

FAILED:
    peer_reset_member(pr);
    torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NONE);
    return -1;
}

static int
peer_event_handshake(struct peer *pr, int event)
{
    if(event & EPOLLOUT) {
        LOG_ALARM("write event occur in handshake state!\n");
    }

    peer_stop_timer(pr);

    if(event & EPOLLIN) {
        if(peer_recv_handshake_msg(pr)) {
            goto FAILED;
        }

        if(peer_mod_event(pr, EPOLLIN)) {
            LOG_ERROR("peer[%s] mod event failed!\n", pr->strfaddr);
            goto FAILED;
        }

        if(peer_send_bitfiled_msg(pr)) {
            LOG_ERROR("peer[%s]send bitfiled failed!\n", pr->strfaddr);
            goto FAILED;
        }

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", pr->strfaddr);
            goto FAILED;
        }

        pr->state = PEER_STATE_CONNECTD;

        return 0;
    }

FAILED:
    peer_reset_member(pr);
    torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NONE);
    return -1;
}

static int
peer_event_connected_recv(struct peer *pr)
{
    int rcvlen = peer_recv_data(pr, pr->pm.rcvbuf+pr->pm.rcvlen,
                                    MAX_BUFFER_LEN - pr->pm.rcvlen);
    if(rcvlen <= 0) {
        LOG_ERROR("peer[%s] recv[%d] error:%s\n", pr->strfaddr, rcvlen, strerror(errno));
        return -1;
    }

    pr->pm.rcvlen += rcvlen;

    while(pr->pm.rcvlen > 0) {

        if(pr->pm.data_transfering) {
            if(peer_recv_left_piece_msg(pr)) {
                return -1;
            }
            continue;
        }

        int res = peer_parser_msg(pr, &pr->pm);

        if(res == -1) {
            LOG_ERROR("peer[%s] parser msg failed!\n", pr->strfaddr);
            return -1;
        } else if(res == -2) {
            LOG_DEBUG("peer[%s] wait more bytes for msg!\n", pr->strfaddr);
            break;
        }
    }

    return 0;
}

static int
peer_event_connected_send(struct peer *pr)
{
    if(peer_send_slice_data(pr)) {
        return -1;
    }

    if(!pr->psm.req_list) {
        peer_mod_event(pr, EPOLLIN);
    }
    return 0;
}

static int
peer_event_connected(struct peer *pr, int event)
{
    if(event & EPOLLIN) {
        if(peer_event_connected_recv(pr)) {
            goto FAILED;
        }
    } 

    if(event & EPOLLOUT) {
        if(peer_event_connected_send(pr)) {
            goto FAILED;
        }
    }

    return 0;

FAILED:
    peer_reset_member(pr);
    torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NORMAL);
    return -1; 
}

static int
peer_event_handle(int event, void *evt_ctx)
{
    struct peer *pr;
    pr = (struct peer *)evt_ctx;

    switch(pr->state) {
        case PEER_STATE_CONNECTING:
            return peer_event_connecting(pr, event);
        case PEER_STATE_SEND_HANDSHAKE:
            return peer_event_handshake(pr, event);
        case PEER_STATE_CONNECTD:
            return peer_event_connected(pr, event);
        default:
            break;
    }

    LOG_ERROR("peer[%s] event[%d] occur in unexpect state[%d]!\n",
                                    pr->strfaddr, event, pr->state);

    return -1;
}

static int
peer_init(struct peer *pr)
{
    pr->heartbeat = time(NULL);
    pr->am_unchoking = 1;
    pr->peer_unchoking =  1;
    pr->having_pieces = NULL;

    pr->psm.sliceoffset = 0;
    pr->psm.piecedata = NULL;
    pr->psm.piecesz = 0;
    pr->psm.req_list = NULL;
    pr->psm.req_tail = &pr->psm.req_list;

    pr->pm.data_transfering = 0;
    pr->pm.rcvlen = 0;

    pr->pm.rcvbuf = GMALLOC(MAX_BUFFER_LEN);
    if(!pr->pm.rcvbuf) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    utils_strf_addrinfo(pr->ipaddr->ip, pr->ipaddr->port, pr->strfaddr, 32);

    if(peer_create_timer(pr)) {
        return -1;
    }

    return 0;
}

int
peer_client_init(struct peer *pr)
{
    pr->state = PEER_STATE_SEND_HANDSHAKE;
    if(peer_init(pr)) {
        goto FAILED;
    }

    if(peer_start(pr)) {
        goto FAILED;
    }

    return 0;
FAILED:
    GFREE(pr->pm.rcvbuf);
    pr->pm.rcvbuf = NULL;
    peer_destroy_timer(pr);
    torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NONE);
    return -1;
}

int
peer_server_init(struct peer *pr)
{
    pr->state = PEER_STATE_NONE;
    if(peer_init(pr)) {
        goto FAILED;
    }

    if(peer_start(pr)) {
        goto FAILED;
    }

    return 0;
FAILED:
    GFREE(pr->pm.rcvbuf);
    pr->pm.rcvbuf = NULL;
    peer_destroy_timer(pr);
    torrent_peer_recycle(pr->tsk, pr, PEER_TYPE_ACTIVE_NONE);
    return -1;
}

