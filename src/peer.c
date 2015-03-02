#include <sys/epoll.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "btype.h"
#include "peer.h"
#include "timer.h"
#include "log.h"
#include "socket.h"
#include "event.h"
#include "bitfield.h"
#include "torrent.h"
#include "utils.h"

extern char peer_id[];

static int peer_reset_member(struct peer *pr);

static int peer_socket_init(struct peer *pr);

static int peer_check_piece_sha1(struct peer *pr);
static int peer_down_complete_slice(struct peer *pr);

static int peer_start(struct peer *pr);
static int peer_connecting_timeout(struct peer *pr);
static int peer_connect_timeout(struct peer *pr);
static int peer_handshake_timeout(struct peer *pr);
static int peer_keepalive_timeout(struct peer *pr);
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

static int peer_send_chocked_msg(struct peer *pr);
static int peer_send_unchocked_msg(struct peer *pr);
static int peer_send_intrested_msg(struct peer *pr);
static int peer_send_notintrested_msg(struct peer *pr);
static int peer_send_have_msg(struct peer *pr, int idx);
static int peer_send_request_msg(struct peer *pr, struct peer_msg *pm);
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

static int peer_parser_msg(struct peer *pr, struct peer_msg *pm);

static int peer_timeout_handle(int evnet, void *evt_ctx);
static int peer_event_handle(int event, void *evt_ctx);

static int peer_event_connecting(struct peer *pr, int event);
static int peer_event_handshake(struct peer *pr, int event);
static int peer_event_bitfield(struct peer *pr, int event);
static int peer_event_connected(struct peer *pr, int event);

static int 
peer_reset_member(struct peer *pr)
{
    peer_destroy_timer(pr);
    peer_del_event(pr);
    pr->isused = 0;

    free(pr->bf.bitmap);
    pr->bf.bitmap = NULL;

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

    int res = socket_tcp_connect(pr->sockid, pr->ip, pr->port);
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
    struct peer_msg *pm = &pr->pm;

    struct slice *tmp = pm->req_list;
    pm->req_list = pm->req_list->next; 
    if(!pm->req_list) {
        pm->req_tail = &pm->req_list;
    }

    *pm->downed_tail = tmp;
    tmp->next = NULL;
    pm->downed_tail = &tmp->next;

    if(!pm->wait_list && !pm->req_list) {
        peer_check_piece_sha1(pr);
    }

    return 0;
}

static int
peer_check_piece_sha1(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    struct peer_msg *pm = &pr->pm;

    if(pm->wait_list || pm->req_list || !pm->downed_list || pm->downed_list->offset != 0) {
        LOG_ERROR("peer[%s] not complete piece!\n");
        return -1;
    }

    char *buffer = malloc(pr->bf.piecesz);
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
        free(sl->data);
    }

    free(pm->downed_list);
    pm->downed_list = NULL;
    pm->downed_tail = &pm->downed_list;

    if(utils_sha1_check(buffer, offset, &pr->tr->tsk->tor.pieces[idx * 20], 20)) {
        LOG_ERROR("peer[%s] piece[%d] sha1 check failed!\n", peeraddr, idx);
        bitfield_peer_garbage_piece(&pr->tr->tsk->bf, idx);
        free(buffer);
        return -1;
    }

    if(torrent_write_piece(pr->tr->tsk, idx, buffer, offset)) {
        LOG_ERROR("peer[%s] write piece[%d]failed!\n", peeraddr, idx);
        free(buffer);
        return -1;
    }

    if(bitfield_local_have(&pr->tr->tsk->bf, idx)) {
        free(buffer);
        return -1;
    }

    LOG_DEBUG("peer[%s] piece[%d] complete!\n", peeraddr, idx);

    free(buffer);
    return 0;
}

static int
peer_connecting_timeout(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_INFO("peer[%s] connecting timeout!\n", peeraddr);

    peer_reset_member(pr);

    return 0;
}

static int
peer_handshake_timeout(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_INFO("peer[%s] handshake timeout!\n", peeraddr);

    peer_reset_member(pr);

    return 0;
}

static int
peer_exchange_bitfield_timeout(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_INFO("peer[%s] exchange bitfield timeout!\n", peeraddr);

    peer_reset_member(pr);

    return 0;
}

static int
peer_keepalive_timeout(struct peer *pr)
{
    if(peer_send_keepalive_msg(pr)) {
        return -1;
    }

    peer_start_timer(pr);

    return 0;
}

static int
peer_connect_timeout(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_INFO("peer[%s]connect timeout!\n", peeraddr);

    peer_reset_member(pr);

    return 0;
}

static int
peer_start(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    if(peer_socket_init(pr)) {
        LOG_ERROR("peer[%s] socket init failed.\n", peeraddr);
        peer_destroy_timer(pr);
        pr->isused = 0;
        return -1;
    }

    if(pr->state == PEER_STATE_SEND_HANDSHAKE) {
        LOG_INFO("peer[%s] connecting...OK\n", peeraddr);

        if(peer_send_handshake_msg(pr)) {
            LOG_ERROR("send peer[%s] handshake msg failed!\n", peeraddr);
            goto FAILED;
        }

        if(peer_add_event(pr, EPOLLIN)) {
            LOG_ERROR("peer[%s] add event failed!\n", peeraddr);
            goto FAILED;
        }

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", peeraddr);
            peer_del_event(pr);
            goto FAILED;
        }

        return 0;
    } else { /* PEER_STATE_CONNECTING */
        LOG_INFO("peer[%s] connecting...in progress\n", peeraddr);

        if(peer_add_event(pr, EPOLLOUT)) {
            LOG_ERROR("peer[%s] add event failed!\n", peeraddr);
            goto FAILED;
        }

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", peeraddr);
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
            return peer_start(pr);
        case PEER_STATE_CONNECTING:
            return peer_connecting_timeout(pr);
        case PEER_STATE_SEND_HANDSHAKE:
            return peer_handshake_timeout(pr);
        case PEER_STATE_EXCHANGE_BITFIELD:
            return peer_exchange_bitfield_timeout(pr);
        case PEER_STATE_CONNECTD:
            return peer_keepalive_timeout(pr);       
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
            return 2600;
        case PEER_STATE_SEND_HANDSHAKE:
            return 2600;
        case PEER_STATE_EXCHANGE_BITFIELD:
            return 2600;
        case PEER_STATE_CONNECTD:
            return 12000;
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
    tp.epfd = pr->tr->tsk->epfd;
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
    tp.epfd = pr->tr->tsk->epfd;
    tp.tmr_hdl = peer_timeout_handle;
    tp.tmr_ctx = pr;

    if(timer_creat(&tp)) {
        LOG_ERROR("peer create timer failed!\n");
        return -1;
    }

    pr->tmrfd = tp.tmrfd;

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

    if(event_add(pr->tr->tsk->epfd, &ep)) {
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

    if(event_mod(pr->tr->tsk->epfd, &ep)) {
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

    if(event_del(pr->tr->tsk->epfd, &ep)) {
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
peer_parser_msg(struct peer *pr, struct peer_msg *pm)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

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
            /* should not recv bitfield msg here! */
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
                return 0;
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
            LOG_ERROR("peer[%s] recv unexpected msgtype[%d]\n", peeraddr, pm->rcvbuf[4]);
    }

    return -1;
}

static int
peer_send_data(struct peer *pr, char *data, int len)
{
    return socket_tcp_send_all(pr->sockid, data, len);
}

static int
peer_recv_data(struct peer *pr, char *rcvbuf, int buflen)
{
    return socket_tcp_recv(pr->sockid, rcvbuf, buflen, 0);
}

static int
peer_send_chocked_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);
    LOG_INFO("peer[%s] send chocked msg\n", peeraddr);

    char msg[5] = {0, 0, 0, 1, PEER_MSG_ID_CHOCKED};
    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }
    return 0;
}

static int
peer_send_unchocked_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);
    LOG_INFO("peer[%s] send unchocked msg\n", peeraddr);

    char msg[5] = {0, 0, 0, 1, PEER_MSG_ID_UNCHOCKED};
    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }
    return 0;
}

static int
peer_send_intrested_msg(struct peer *pr)
{
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
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);
    LOG_INFO("peer[%s] send notintrested msg\n", peeraddr);

    char msg[5] = {0, 0, 0, 1, PEER_MSG_ID_NOTINSTRESTED};
    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }
    return 0;
}

static int
peer_send_have_msg(struct peer *pr, int idx)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);
    LOG_INFO("peer[%s] send have msg[%d]\n", peeraddr, idx);

    char msg[5] = {0, 0, 0, 1, PEER_MSG_ID_NOTINSTRESTED};
    if(peer_send_data(pr, msg, sizeof(msg))) {
        return -1;
    }
    return 0;
}

static int
peer_send_request_msg(struct peer *pr, struct peer_msg *pm)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    if(!pm->wait_list) {
       if(bitfield_get_request_piece(&pr->tr->tsk->bf, &pr->bf, pm)) {
            LOG_DEBUG("peer[%s] have no piece for us!\n", peeraddr);
            return -1;
        }
    }

    struct slice *tmp = pm->wait_list;
    pm->wait_list = pm->wait_list->next;
    *pm->req_tail = tmp;
    tmp->next = NULL;
    pm->req_tail = &tmp->next;

    struct slice *sl;
    for(sl = pm->req_list; sl; sl = sl->next) {

        LOG_DEBUG("peer[%s] send request msg[%d,%d,%d]\n",
                  peeraddr, sl->idx, sl->offset, sl->slicesz);

        char msg[17] = {0, 0, 0, 13, PEER_MSG_ID_REQUEST};

        int idx = socket_htonl(sl->idx);
        memcpy(msg+5, &idx, 4);

        int offset = socket_htonl(sl->offset);
        memcpy(msg+9, &offset, 4);

        int sz = socket_htonl(sl->slicesz);
        memcpy(msg+13, &sz, 4);

        if(peer_send_data(pr, msg, sizeof(msg))) {
            LOG_ERROR("peer[%s] send request msg[%d,%d,%d] failed\n",
                      peeraddr, sl->idx, sl->offset, sl->slicesz);
            return -1;
        }
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
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    char buf[4];
    memset(buf, 0, sizeof(buf));

    if(peer_send_data(pr, buf, sizeof(buf))) {
        LOG_ERROR("peer[%s] send keepalive failed:%s\n",peeraddr, strerror(errno));
        return -1;
    }

    LOG_INFO("peer[%s] send keepalive msg ok!\n", peeraddr);

    return 0;
}

static int
peer_send_handshake_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    char handshake[68], *s = handshake;
    s += snprintf(handshake, sizeof(handshake), "%c%s", 19, "BitTorrent protocol");

    char reserved[8];
    memset(reserved, 0, sizeof(reserved));
    memcpy(s, reserved, sizeof(reserved));
    s += sizeof(reserved);

    memcpy(s, pr->tr->tsk->tor.info_hash, SHA1_LEN);
    s += SHA1_LEN;
    memcpy(s, peer_id, PEER_ID_LEN);

    if(peer_send_data(pr, handshake, sizeof(handshake))) {
        LOG_ERROR("peer[%s] send handshake msg: %s\n", peeraddr, strerror(errno));
        return -1;
    }

    return 0;
}

/* 19+bt_proto_str+peerid */
static int
peer_recv_handshake_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    char handshake[68]; 

    int rcvlen = socket_tcp_recv(pr->sockid, handshake, sizeof(handshake), 0);
    if(rcvlen != sizeof(handshake)) {
        LOG_ERROR("peer[%s] recv handshake failed[%d:%s]!\n", peeraddr, rcvlen, strerror(errno));
        return -1;
    }

    if(handshake[0] != (unsigned char)19
                    || memcmp(handshake+1, "BitTorrent protocol", 19)
                    || memcmp(handshake+28, pr->tr->tsk->tor.info_hash, PEER_ID_LEN)) {
        LOG_ERROR("peer[%s] recv handshake content invalid!\n", peeraddr);
        return -1;
    }

    LOG_DEBUG("peer[%s] handshake ok, client peerid:%.10s\n", peeraddr, handshake+48);

    return 0;
}

static int
peer_send_bitfiled_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    struct bitfield *bf;
    bf = &pr->tr->tsk->bf;

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
        LOG_ERROR("peer[%s] send bitfield failed:%s\n", strerror(errno));
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

    LOG_INFO("peer[%s] send bitfield ok!\n", peeraddr);

    return 0;
}

/* len_pre+id+bitmap */
static int
peer_recv_bitfield_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    if(pr->bf.bitmap) {
        LOG_ERROR("peer[%s] bitmap exist when recv bitfield msg!\n", peeraddr);
        return -1;
    }
    
    int msglen = 5 + pr->tr->tsk->bf.nbyte; 
    char rcvbuf[msglen];
    if(peer_recv_data(pr, rcvbuf, msglen) != msglen) {
        LOG_ERROR("peer[%s] recv data failed[%d][%s]!\n", peeraddr, msglen, strerror(errno));
        return -1;
    }

    int len_pre;
    memcpy(&len_pre, rcvbuf, 4);
    len_pre = socket_ntohl(len_pre);

    if(rcvbuf[4] != PEER_MSG_ID_BITFIELD || len_pre-1 != pr->tr->tsk->bf.nbyte) {
        LOG_DUMP(rcvbuf, msglen, "peer[%s] bitfield[%d]:", peeraddr, msglen);
        LOG_ERROR("peer[%s] read bitfield invalid\n", peeraddr);
        return -1;
    }

    if(bitfield_create(&pr->bf, pr->tr->tsk->bf.npieces, pr->tr->tsk->bf.piecesz, pr->tr->tsk->bf.totalsz)) {
        LOG_ERROR("peer[%s] bitfield create failed!\n", peeraddr);
        return -1;
    }

    if(bitfield_dup(&pr->bf, rcvbuf+5, len_pre-1)) {
        LOG_ERROR("peer[%s] bitfield dup failed!\n", peeraddr);
        return -1;
    }

    return 0;
}

/* len_pre+id */
static int
peer_recv_choked_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_INFO("peer[%s] recv chocked msg!\n", peeraddr);
    pr->peer_unchoking = 0;
    peer_send_intrested_msg(pr);

    struct peer_msg *pm = &pr->pm;
    if(pm->req_list) {
        *pm->req_tail = pm->wait_list;
        pm->wait_list = pm->req_list;
        pm->req_list = NULL;
        pm->req_tail = &pm->req_list;
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
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_INFO("peer[%s] recv unchocked msg!\n", peeraddr);
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
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_INFO("peer[%s] recv intrested msg!\n", peeraddr);
    pr->peer_interested = 1;
    pr->am_unchoking = 1;
    peer_send_unchocked_msg(pr);

    struct peer_msg *pm;
    pm = &pr->pm;
    pm->rcvlen -= 5;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+5, pm->rcvlen);
    }

    return 0;
}

/* len_pre+id */
static int
peer_recv_notintrested_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_INFO("peer[%s] recv intrested msg!\n", peeraddr);
    pr->peer_interested = 0;

    struct peer_msg *pm;
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
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    struct peer_msg *pm;
    pm = &pr->pm;

    int idx;
    memcpy(&idx, pm->rcvbuf+5, 4);
    idx = socket_ntohl(idx);

    pm->rcvlen -= 9;
    if(pm->rcvlen) {
        memmove(pm->rcvbuf, pm->rcvbuf+9, pm->rcvlen);
    }

    LOG_INFO("peer[%s] recv have msg[%d]!\n", peeraddr, idx);

    if(!bitfield_peer_have(&pr->tr->tsk->bf, &pr->bf, idx) && !pr->am_interested) {
        peer_send_intrested_msg(pr);
    }

    return 0;
}

/* len_pre+id+idx+offset+size */
static int
peer_recv_request_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    struct peer_msg *pm;
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

    LOG_INFO("peer[%s] recv request msg[%d,%d,%d]!\n", peeraddr, idx, offset, size);

    return 0;
}

/* len_pre+id+idx+offset+size */
static int
peer_recv_cancel_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    struct peer_msg *pm;
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

    LOG_INFO("peer[%s] recv cancel msg[%d,%d,%d]!\n", peeraddr, idx, offset, size);

    return 0;
}

/* len_pre=0000 */
static int
peer_recv_keepalive_msg(struct peer *pr)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    LOG_DEBUG("peer[%s] recv keepalive msg!\n", peeraddr);

    struct peer_msg *pm;
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
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

#if 0
    LOG_DEBUG("peer[%s] recv left piece msg block[%d+%d][%d,%d]!\n",
            peeraddr, pr->pm.req_list->downsz, pr->pm.rcvlen,
            pr->pm.req_list->offset, pr->pm.req_list->slicesz);
#endif

    struct peer_msg *pm = &pr->pm;
    int totalsz = pm->req_list->downsz + pm->rcvlen;

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
                peeraddr, pm->req_list->idx, pm->req_list->offset, pm->req_list->slicesz);
#endif
        peer_down_complete_slice(pr);
        peer_send_request_msg(pr, pm);

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
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    struct peer_msg *pm;
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

    if(len_pre - 9 != pm->req_list->slicesz
                   || pm->req_list->offset != offset || pm->req_list->idx != idx) {
        LOG_ERROR("peer[%s] recv not correct piece msg[%d,%d]!\n", peeraddr, idx, offset);
        return -1;
    }

#if 0
    LOG_INFO("peer[%s] recv piece msg[%d,%d] block[%d,%d]!\n",
            peeraddr, idx, offset, datasz, pm->req_list->slicesz);
#endif

    pm->req_list->data = malloc(pm->req_list->slicesz);
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
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

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
            LOG_INFO("peer[%s] connect: %s\n", peeraddr, strerror(errNo));
            goto FAILED;
        }

        LOG_INFO("peer[%s] connect ok.\n", peeraddr);

        if(peer_send_handshake_msg(pr)) {
            LOG_ERROR("peer[%s] send handshake msg failed!\n", peeraddr);
            goto FAILED;
        }

        if(peer_mod_event(pr, EPOLLIN)) {
            LOG_ERROR("peer[%s] mod event failed!\n", peeraddr);
            goto FAILED;
        }

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", peeraddr);
            goto FAILED;
        }

        LOG_INFO("peer[%s] send handshake msg ok!\n", peeraddr);

        pr->state = PEER_STATE_SEND_HANDSHAKE;

        return 0;
    }

FAILED:
    peer_reset_member(pr);
    return -1;
}

static int
peer_event_handshake(struct peer *pr, int event)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    if(event & EPOLLOUT) {
        LOG_ALARM("write event occur in handshake state!\n");
    }

    peer_stop_timer(pr);

    if(event & EPOLLIN) {
        if(peer_recv_handshake_msg(pr)) {
            goto FAILED;
        }

        if(peer_mod_event(pr, EPOLLIN)) {
            LOG_ERROR("peer[%s] mod event failed!\n", peeraddr);
            goto FAILED;
        }

        if(peer_send_bitfiled_msg(pr)) {
            LOG_ERROR("peer[%s]send bitfiled failed!\n", peeraddr);
            goto FAILED;
        }

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", peeraddr);
            goto FAILED;
        }

        pr->state = PEER_STATE_EXCHANGE_BITFIELD;

        return 0;
    }

FAILED:
    peer_reset_member(pr);
    return -1;
}

static int
peer_event_bitfield(struct peer *pr, int event)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    peer_stop_timer(pr);

    if(event & EPOLLIN) {
        if(peer_recv_bitfield_msg(pr)) {
            LOG_ERROR("peer[%s] recv bitfield failed!\n");
            goto FAILED;
        }

        LOG_DUMP(pr->bf.bitmap, pr->bf.nbyte, "peer[%s]bitfield[%d]:", peeraddr, pr->bf.nbyte);
        LOG_INFO("peer[%s] recv bitfield ok!\n", peeraddr);

        if(bitfield_intrested(&pr->tr->tsk->bf, &pr->bf)) {
            if(peer_send_intrested_msg(pr)) {
                LOG_ERROR("peer[%s] send intrested msg failed!\n", peeraddr);
                goto FAILED;
            } 
            LOG_INFO("peer[%s] send intrested msg ok!\n", peeraddr);
        }

        pr->state = PEER_STATE_CONNECTD;

        if(peer_start_timer(pr)) {
            LOG_ERROR("peer[%s] start timer failed!\n", peeraddr);
        }

        return 0;
    }

FAILED:
    peer_reset_member(pr);
    return -1;
}

static int
peer_event_connected(struct peer *pr, int event)
{
    char peeraddr[32];
    utils_strf_addrinfo(pr->ip, pr->port, peeraddr, 32);

    if(event & EPOLLIN) {

        int rcvlen = peer_recv_data(pr, pr->pm.rcvbuf+pr->pm.rcvlen,
                                        sizeof(pr->pm.rcvbuf) - pr->pm.rcvlen);
        if(rcvlen <= 0) {
            LOG_ERROR("peer[%s] recv[%d] error:%s\n", peeraddr, rcvlen, strerror(errno));
            goto FAILED;
        }

        pr->pm.rcvlen += rcvlen;

        while(pr->pm.rcvlen > 0) {

            if(pr->pm.data_transfering) {
                peer_recv_left_piece_msg(pr);
                continue;
            }

            int res = peer_parser_msg(pr, &pr->pm);

            if(res == -1) {
                LOG_ERROR("peer[%s] parser msg failed!\n", peeraddr);
                goto FAILED;
            } else if(res == -2) {
                LOG_DEBUG("peer[%s] wait more bytes for msg!\n", peeraddr);
                break;
            }
        }
    }

    return 0;

FAILED:
    peer_reset_member(pr);
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
        case PEER_STATE_EXCHANGE_BITFIELD:
            return peer_event_bitfield(pr, event);
        case PEER_STATE_CONNECTD:
            return peer_event_connected(pr, event);
        default:
            LOG_ERROR("event[%d] occur in unexpect state[%d]!\n", event, pr->state);
            break;
    }

    return -1;
}

int
peer_init(struct tracker *tr, char *addrinfo)
{
    int i;
    for(i = 0; i < MAX_PEER_NUM && tr->pr[i].isused; i++) {
        /* nothing */
    }

    if(i == MAX_PEER_NUM) {
        return -1;
    }

    struct peer *pr = &tr->pr[i];

    memset(pr, 0, sizeof(*pr));
    pr->tr = tr;
    pr->ip = *(int *)addrinfo;
    pr->port = *(unsigned short *)(addrinfo+4);
    pr->am_unchoking = 1;
    pr->peer_unchoking =  1;
    pr->state = PEER_STATE_NONE;

    if(peer_create_timer(pr)) {
        return -1;
    }

    if(peer_start_timer(pr)) {
        return -1;
    }

    pr->isused = 1;

    return 0;
}

