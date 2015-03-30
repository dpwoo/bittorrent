#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include "type.h"
#include "log.h"
#include "socket.h"
#include "event.h"
#include "mempool.h"

struct usr_cmd {
    int epfd, fd;
    uint16 port;
    struct torrent_task *tsk;
};

#define CLI  "USAGE:\n" \
             "1)HELP\n" \
             "2)MEMDUMP SECOND SIZE\n" \
             "3)LOG LEVEL FMT\n" \
             "4)DUMP PIECE\n" \
             "5)DUMP BITMAP\n"
             
static int cmd_event_handle(int event, void *evt_ctx);
static int cmd_add_event(struct usr_cmd *uc, int event);
static int cmd_msg_parser(struct usr_cmd *uc, char *msgbuf, int bufsz);

static int
cmd_add_event(struct usr_cmd *uc, int event)
{
    struct event_param ep;
    ep.event = event;
    ep.fd = uc->fd;
    ep.evt_hdl = cmd_event_handle;
    ep.evt_ctx = uc;

    if(event_add(uc->epfd, &ep)) {
        LOG_ERROR("peer add event failed!\n");
        return -1;
    }

    return 0;
}

int
cmd_init(struct torrent_task *tsk, int epfd)
{
    struct usr_cmd *uc;
    if(!(uc = GCALLOC(1, sizeof(*uc)))) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    uc->epfd = epfd;
    uc->tsk = tsk;
    uc->fd = socket_udp_create(); 
    if(uc->fd < 0) {
        LOG_ERROR("create udp sock failed:%s\n", strerror(errno));
        GFREE(uc);
        return -1;
    }

    if(set_socket_unblock(uc->fd)) {
        close(uc->fd);
        return -1;
    }

    int i;
    for(i = 6881; i < 65535; i++) {
        if(!socket_udp_bind(uc->fd, 0, socket_htons(i))) {
            break;
        }
    }

    if(i == 65535) {
        LOG_ERROR("bind udp failed!\n");
        close(uc->fd);
        GFREE(uc);
        return -1;
    }

    uc->port = i;
    LOG_DEBUG("udp listen %d\n", i);

    if(cmd_add_event(uc, EPOLLIN)) {
        close(uc->fd);
        GFREE(uc);
        return -1;
    }

    return 0;
}

static int
cmd_msg_parser(struct usr_cmd *uc, char *msgbuf, int bufsz)
{
    if(!memcmp(msgbuf, "HELP", 4)) {
        fprintf(stderr, "%s\n", CLI);
        return 0;
    }

    if(!memcmp(msgbuf, "MEMDUMP", 7)) {
        char *ptr, *s = msgbuf+7;
        errno = 0;

        int second = strtol(s, &ptr, 10);
        if(errno || second < -1) {
            LOG_ERROR("invalid log second setting[%d]!\n", second);
            return -1;
        }
        
        s = ptr;
        int size = strtol(s, &ptr, 10);
        if(errno || size < -1) {
            LOG_ERROR("invalid log size setting[%d]!\n", size);
            return -1;
        }

        MEM_DUMP(0, second, size);
        return 0;
    }

    if(!memcmp(msgbuf, "LOG", 3)) {
        char *ptr, *s = msgbuf+3;
        errno = 0;

        int level = strtol(s, &ptr, 10);
        if(errno || (level < -1 || level > 4)) {
            LOG_ERROR("invalid log level setting[%d]!\n", level);
            return -1;
        }
        
        s = ptr;
        int fmt = strtol(s, &ptr, 10);
        if(errno || fmt < 0 || fmt > 1) {
            LOG_ERROR("invalid log fmt setting[%d]!\n", fmt);
            return -1;
        }

        set_log_level(level, fmt);
    }

    if(!memcmp(msgbuf, "DUMP PIECE", 10)) {
        int64 totalsz = 0;

        fprintf(stderr, "\nDUMP PIECES:\n");
        struct pieces *p;
        for(p = uc->tsk->bf.stoped_list; p; p = p->next) {
            totalsz += uc->tsk->bf.piecesz;
            fprintf(stderr, "piece[%d,%d]\n", p->idx, p->wait_list->offset);
        }

        fprintf(stderr, "\nDUMP PEER:\n");
        int i, used = 0, now = time(NULL);
        for(i = 0; i < MAX_PEER_NUM; i++) {
            struct peer *pr = &uc->tsk->pr[i];
            if(pr->isused && pr->state == PEER_STATE_CONNECTD) {
                fprintf(stderr, "peer[%s][%.8s][%d][rcvbuf=%p,sndbuf=%p]\n",
                        pr->strfaddr, pr->peerid, now - pr->start_time,
                        pr->pm.piecebuf, pr->psm.piecedata);
                used++;
            } else if(pr->pm.piecebuf || pr->psm.piecedata) {
                fprintf(stderr, "memory leek!!!!:[%s][rcvbuf=%p,sndbuf=%p]\n",
                        pr->strfaddr, pr->pm.piecebuf, pr->psm.piecedata);
            }
        }

        fprintf(stderr, "PIECE[%d] totalsz = %lld, peer[%d]\n\n",
                uc->tsk->bf.piecesz, totalsz, used);
    }

    if(!memcmp(msgbuf, "DUMP BITMAP", 11)) {
        int i;
        for(i = 0; i < uc->tsk->bf.nbyte; i++) {
            fprintf(stderr, "0x%02X ", (uint8)uc->tsk->bf.bitmap[i]);
        }
        fprintf(stderr, "\n");
    }

    return 0;
}

static int
cmd_event_handle(int event, void *evt_ctx)
{
    struct usr_cmd *uc;
    uc = (struct usr_cmd *)evt_ctx;

    char buffer[128];

    struct sockaddr sa;
    socklen_t sl = sizeof(sa);
    int rcvsz = socket_udp_recvfrom(uc->fd, buffer, sizeof(buffer), 0, &sa, &sl); 
    if(rcvsz <= 0) {
        return -1;
    }

    cmd_msg_parser(uc, buffer, rcvsz);

    return 0;
}

