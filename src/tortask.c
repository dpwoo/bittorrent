#include <sys/epoll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "btype.h"
#include "log.h"
#include "torrent.h"
#include "tortask.h"
#include "timer.h"
#include "bitfield.h"
#include "tracker.h"
#include "peer.h"
#include "utils.h"
#include "mempool.h"
#include "socket.h"

static int torrent_listen(struct torrent_task *tsk);
static int torrent_stop_timer(struct torrent_task *tsk);
static int torrent_start_timer(struct torrent_task *tsk);
static int torrent_create_timer(struct torrent_task *tsk);
static int torrent_get_free_peer(struct torrent_task *tsk, struct peer **pr);
static int torrent_peer_notify(struct torrent_task *tsk);
static int torrent_tracker_announce(struct torrent_task *tsk);
static int torrent_peer_init(struct torrent_task *tsk);
static int torrent_init_tracker_annoucelist(struct torrent_task *tsk);
static int torrent_find_peer_addrinfo(struct torrent_task *tsk, struct peer_addrinfo *ai);
static int torrent_free_inactive_peer_addrinfo(struct torrent_task *tsk, int idx);
static int torrent_add_event(struct torrent_task *tsk, int event);
static int torrent_del_event(struct torrent_task *tsk);

static int torrent_listen_handle(int event, void *evt_ctx);
static int torrent_timeout_handle(int event, void *evt_ctx);

int
torrent_task_init(struct torrent_task *tsk, int epfd, char *torfile)
{
	memset(tsk, 0, sizeof(*tsk));
	
	tsk->epfd = epfd;
	tsk->tmrfd = -1;
    tsk->listen_port = 6881;

    tsk->tr_inactive_list_tail = &tsk->tr_inactive_list;

    int i;
    for(i = 0; i < PEER_TYPE_ACTIVE_NUM; i++) {
        tsk->pr_list[i].tail = &tsk->pr_list[i].head;
    }

	if(torrent_create_timer(tsk)) {
		return -1;
	}

    if(torrent_file_parser(torfile, &tsk->tor)) {
        LOG_ERROR("parser %s failed!\n", torfile);
        return -1;
	}

	if(torrent_info_parser(&tsk->tor)) {
		LOG_ERROR("parser torrent info failed!\n");
		return -1;
	}

    if(bitfield_create(&tsk->bf, tsk->tor.pieces_num, tsk->tor.piece_len, tsk->tor.totalsz)) {
        LOG_ERROR("bitfield creat failed!\n");
        return -1;
    }

    if(torrent_create_downfiles(tsk)) {
        LOG_ERROR("torrent create downfile failed!\n");
        return -1;
    }

    torrent_check_downfiles_bitfield(tsk);

	if(torrent_init_tracker_annoucelist(tsk)) {
		return -1;
	}

    if(torrent_listen(tsk)) {
        LOG_ERROR("torrent listen failed!\n");
        return -1;
    }

    if(torrent_add_event(tsk, EPOLLIN)) {
        return -1;
    }

	if(torrent_start_timer(tsk)) {
		return -1;
	}
	
	return 0;
}

static int
torrent_listen(struct torrent_task *tsk)
{
    int sock = socket_tcp_create();
    if(sock < 0) {
        return -1;
    }

    if(set_socket_unblock(sock)) {
        close(sock);
        return -1;
    }

    if(set_socket_opt(sock)) {
        close(sock);
        return -1;
    }

    int i, ip = 0;
    for(i = 6881; i < 65535; i++) {
        uint16 bport = socket_htons(i);
        if(!socket_tcp_bind(sock, ip, bport)) {
            break;
        }
    }

    if(i >= 65535) {
        close(sock);
        return -1;
    }

    if(socket_tcp_listen(sock, 1024)) {
        close(sock);
        return -1;
    }

    tsk->listenfd = sock;
    tsk->listen_port = i; 

    LOG_DEBUG("listen port : %hu\n", tsk->listen_port);

    return 0;
}

static int
torrent_stop_timer(struct torrent_task *tsk)
{
    if(tsk->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.tmrfd = tsk->tmrfd;
    
    if(timer_stop(&tp)) {
        LOG_ERROR("torrent stop timer failed!\n");
        return -1;
    }

    return 0;
}

static int
torrent_start_timer(struct torrent_task *tsk)
{
    if(tsk->tmrfd < 0) {
        return -1;
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.tmrfd = tsk->tmrfd;
    tp.time = 100;
    tp.interval = 0;

    if(timer_start(&tp)) {
        LOG_ERROR("torrent start timer failed!\n");
        return -1;
    }

    return 0;
}

static int
torrent_create_timer(struct torrent_task *tsk)
{
    if(tsk->tmrfd >= 0) {
        LOG_ALARM("torrent tmrfd is [%d] when create timer!\n", tsk->tmrfd);
    }

    struct timer_param tp;
    memset(&tp, 0, sizeof(tp));
    tp.epfd = tsk->epfd;
    tp.tmr_hdl = torrent_timeout_handle;
    tp.tmr_ctx = tsk;

    if(timer_creat(&tp)) {
        LOG_ERROR("torrent create timer failed!\n");
        return -1;
    }

    tsk->tmrfd = tp.tmrfd;

    return 0;
}

static int
torrent_add_event(struct torrent_task *tsk, int event)
{
    struct event_param ep;
    ep.event = event;
    ep.fd = tsk->listenfd;
    ep.evt_hdl = torrent_listen_handle;
    ep.evt_ctx = tsk;

    if(event_add(tsk->epfd, &ep)) {
        LOG_ERROR("peer add event failed!\n");
        return -1;
    }

    return 0;
}

static int
torrent_del_event(struct torrent_task *tsk)
{
    if(tsk->listenfd < 0) {
        return -1;
    }

    struct event_param ep;
    memset(&ep, 0, sizeof(ep));
    ep.fd = tsk->listenfd;

    if(event_del(tsk->epfd, &ep)) {
        LOG_ERROR("peer del event failed!\n");
        return -1;
    }

    close(tsk->listenfd);
    tsk->listenfd = -1;

    return 0;
}

static int
torrent_find_peer_addrinfo(struct torrent_task *tsk, struct peer_addrinfo *ai)
{
    struct peer_addrinfo *iter;

    int i;
    for(i = 0; i < PEER_TYPE_ACTIVE_NUM; i++) {
        iter = tsk->pr_list[i].head;
        for(; iter; iter = iter->next) {
            if(ai->ip == iter->ip && ai->port == iter->port) {
                return -1;
            }
        }
    }

    for(i = 0; i < MAX_PEER_NUM; i++) {
        iter = tsk->pr[i].ipaddr;
        if(tsk->pr[i].isused && iter &&  ai->ip == iter->ip && ai->port == iter->port) {
            return -1;
        }
    }

    return 0;
}

/* TODO: should delete ourself ip+port */
int
torrent_add_peer_addrinfo(struct torrent_task *tsk, char *peer)
{
	struct peer_addrinfo *ai;

	ai = GCALLOC(1, sizeof(*ai));
	if(!ai) {
		LOG_ERROR("out of memory!\n");
		return -1;
	}

	memcpy(&ai->ip, peer, 4);
	memcpy(&ai->port, peer+4, 2);
    ai->next_connect_time = time(NULL);

    if(torrent_find_peer_addrinfo(tsk, ai)) {
        GFREE(ai);
        return -1;
    }

    *tsk->pr_list[PEER_TYPE_ACTIVE_NONE].tail = ai;
    tsk->pr_list[PEER_TYPE_ACTIVE_NONE].tail = &ai->next;

	return 0;
}

static int
torrent_get_free_peer(struct torrent_task *tsk, struct peer **pr)
{
	int i;
	for(i = 0; i < MAX_PEER_NUM; i++) {
		if(!tsk->pr[i].isused) {
			*pr = &tsk->pr[i];
			return 0;
		}
	}

	return -1;
}

static int
torrent_init_tracker_annoucelist(struct torrent_task *tsk)
{
	int i;
	for(i = 0; i < tsk->tor.tracker_num; i++) {
		struct tracker *tr;
		tr = GCALLOC(1, sizeof(*tr));
		if(!tr) {
			LOG_ERROR("out of memory!\n");
			continue;
		}

		if(utils_url_parser(tsk->tor.tracker_url[i], &tr->tp)) {
			GFREE(tr);
			continue;
		}

        LOG_DEBUG("tracker(%s)[%s:%s]\n",
            tr->tp.prot_type == TRACKER_PROT_UDP ? "UDP" : "HTTP", tr->tp.host, tr->tp.port);

		*tsk->tr_inactive_list_tail = tr;
		tsk->tr_inactive_list_tail = &tr->next;
	}

	if(!tsk->tr_inactive_list) {
		LOG_ERROR("torrent have no tracker announce list!\n");
		return -1;
	}

	return 0;
}

static int
torrent_peer_init(struct torrent_task *tsk)
{
	if(tsk->npeer >= MAX_PEER_NUM) {
		return 0;
	}

    time_t now = time(NULL);

    int k, i;
    struct peer_addrinfo *tmp, **pai;

    for(k = 0; k < PEER_TYPE_ACTIVE_NUM; k++) {
        pai= &tsk->pr_list[k].head;
        for(i = 0; i < 5 && *pai; i++) {
            if((*pai)->next_connect_time > now) {
                pai = &(*pai)->next;
                i--;
                continue;
            }

            struct peer *pr;
            if(torrent_get_free_peer(tsk, &pr)) {
                return 0;
            }

            tmp = *pai; 
            *pai = tmp->next;
            if(!tmp->next) {
                tsk->pr_list[k].tail = pai;
            }

            tmp->next = NULL;
            pr->ipaddr = tmp; 
            pr->tsk = tsk;
            pr->isused = 1;
            peer_init(pr);
        }
    }

	return 0;
}

int
torrent_peer_recycle(struct torrent_task *tsk, struct peer *pr, int how_active)
{
    if(pr->ipaddr->client) {
        GFREE(pr->ipaddr);
        pr->isused = 0;
        pr->ipaddr = NULL;
        return 0;
    }

    int index = pr->ipaddr->downsz > 0 ? PEER_TYPE_ACTIVE_SUPER :
                (how_active != PEER_TYPE_ACTIVE_NONE ? PEER_TYPE_ACTIVE_NORMAL :
                PEER_TYPE_ACTIVE_NONE);

    int next_time = index == PEER_TYPE_ACTIVE_SUPER ? time(NULL) + 60 :
               (index == PEER_TYPE_ACTIVE_NORMAL ? time(NULL) + 60*4 :
               time(NULL) + 60*8);

    pr->ipaddr->next_connect_time = next_time;
    *tsk->pr_list[index].tail = pr->ipaddr;
    tsk->pr_list[index].tail = &pr->ipaddr->next;

    pr->isused = 0;
    pr->ipaddr = NULL;

    return 0;
}

int
torrent_tracker_recycle(struct torrent_task *tsk, struct tracker *tr, int isactive)
{
    if(isactive == 2) { /* can't resolve dns */
        GFREE(tr);
        return 0;
    }

    if(isactive) {
        tr->next = tsk->tr_active_list;
        tsk->tr_active_list = tr;
    } else {
        tr->annouce_time = time(NULL) + 60*2; /* 60*2 second try angain */
        *tsk->tr_inactive_list_tail = tr;
        tsk->tr_inactive_list_tail = &tr->next;
        *tsk->tr_inactive_list_tail = NULL;
    }

	return 0;
}

static int
torrent_free_inactive_peer_addrinfo(struct torrent_task *tsk, int idx)
{
    if(idx < 0 || idx >= PEER_TYPE_ACTIVE_NUM) {
        return -1;
    }

    struct peer_addrinfo *tmp, **ai;
    ai = &tsk->pr_list[idx].head;
    while(*ai) {
        if((*ai)->downsz <= 0) {
            tmp = *ai;
            *ai = tmp->next;
            if(!tmp->next) {
                tsk->pr_list[idx].tail = ai;
            }
            GFREE(tmp);
            continue;
        }
        ai = &(*ai)->next;
    }

    return 0;
}

static int
torrent_tracker_announce(struct torrent_task *tsk)
{
	time_t now = time(NULL);

	struct tracker *tmp, **tr;
	for(tr = &tsk->tr_active_list; *tr; ) {
		if(now >= (*tr)->annouce_time) {
            tmp = *tr;
            *tr = tmp->next;
            tmp->next = NULL;

            if(tmp->tp.prot_type == TRACKER_PROT_HTTP) {
                tracker_http_announce(tmp);
            } else {
                tracker_udp_announce(tmp);
            }

            /* re_announce time, we remove inactive peer */
            torrent_free_inactive_peer_addrinfo(tsk, PEER_TYPE_ACTIVE_NONE);

            continue;
		}
        tr = &(*tr)->next;
	}

	for(tr = &tsk->tr_inactive_list; *tr; tr = &(*tr)->next) {
		if(now < (*tr)->annouce_time) {
            continue;
		}

        tmp = *tr;
        *tr = tmp->next;
        if(!tmp->next) {
            tsk->tr_inactive_list_tail = tr;
        }

        tmp->tsk = tsk;
        tmp->next = NULL;

        if(tmp->tp.prot_type == TRACKER_PROT_HTTP) {
            tracker_http_announce(tmp);
        } else {
            tracker_udp_announce(tmp);
        }

        /* just take one */
        break;
	}

	return 0;
}

int
torrent_add_having_piece(struct torrent_task *tsk, int idx)
{
    struct pieces *p;
    if((p = GCALLOC(1, sizeof(*p)))) {
        p->idx = idx;
        p->next = tsk->havelist;
        tsk->havelist = p;
    }
    return 0;
}

static int
torrent_peer_notify(struct torrent_task *tsk)
{
    if(!tsk->havelist) {
        return 0;
    }

    struct pieces *idx = tsk->havelist;
    tsk->havelist = idx->next;
    idx->next = NULL;

	int i;
	for(i = 0; idx && i < MAX_PEER_NUM; i++) {
		if(tsk->pr[i].isused && tsk->pr[i].state == PEER_STATE_CONNECTD) {
            struct pieces *p = GCALLOC(1, sizeof(*p));
            if(p) {
                p->idx = idx->idx;
                p->next = tsk->pr[i].having_pieces;
                tsk->pr[i].having_pieces = p;
                peer_modify_timer_time(&tsk->pr[i], 5); 
            }
        }
    }

    GFREE(idx);

	return 0;
}

static int
torrent_listen_handle(int event, void *evt_ctx)
{
	struct torrent_task *tsk;
	tsk = (struct torrent_task *)evt_ctx;

    uint16 port;
    int ip, clisock;

    clisock = socket_tcp_accept(tsk->listenfd, &ip, &port);
    if(clisock < 0) {
        LOG_ERROR("accept %hu failed:%s\n", tsk->listen_port, strerror(errno));
        return -1;
    }

    struct peer_addrinfo *ai;
    ai = GCALLOC(1,sizeof(*ai));
    if(!ai) {
        close(clisock);
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    ai->client = 1;
    ai->ip = ip;
    ai->port = port;

    char peeraddr[32];
    utils_strf_addrinfo(ip, port, peeraddr, 32);
    LOG_INFO("peer[%s] connect us!\n", peeraddr);

    struct peer *pr;
    if(torrent_get_free_peer(tsk, &pr)) {
        LOG_DEBUG("have no free peer slot for incoming client!\n");
        close(clisock);
        GFREE(ai);
        return -1;
    }

    pr->isused = 1;
    pr->sockid = clisock;
    pr->ipaddr = ai;
    pr->tsk = tsk;

    peer_init(pr);

    return 0;
}

static int
torrent_timeout_handle(int event, void *evt_ctx)
{
	struct torrent_task *tsk;
	tsk = (struct torrent_task *)evt_ctx;

	torrent_stop_timer(tsk);

	if(tsk->leftpieces > 0) {
		torrent_peer_init(tsk);	
	}

	torrent_peer_notify(tsk);

	torrent_tracker_announce(tsk);

	torrent_start_timer(tsk);

	return 0;
}

