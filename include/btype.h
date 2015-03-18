/*
 *  file btype.h
 *  author dpwoo
 *  date  2015-01-17 
 */

#ifndef BTYPE_H_
#define BTYPE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "type.h"

#define DOWN_TYPE_TYPE_SINGLE 1
#define DOWN_TYPE_TYPE_MANY 2

#define PEER_ID_LEN 20
#define SHA1_LEN 20
#define MAX_TRACKER_NUM 32
#define MAX_PEER_NUM 50 
#define SLICE_SZ (16*1024)
#define MTU_SZ (1400)

enum {
    BENC_TYPE_NONE = 0,
    BENC_TYPE_INT ,
    BENC_TYPE_STRING ,
    BENC_TYPE_LIST ,
    BENC_TYPE_DICT ,
};

struct benc_type {
    int type;
    union{
        int64 i;
        struct {
            int len;
            char *s;
        }str;
        /* for list and dictionary */
        struct {
            char *begin, *end;
            int nlist, alloced;
            struct benc_type *vals;
        }list;
   }val;
};

struct single_file {
    char *subdir;
    char *pathname;
    int64 file_size;
};

struct multi_files {
    int files_num;
    struct single_file *files;
};

struct torrent_file {
    char *torfile;
    struct benc_type bt;

    int piece_len;
    char *pieces;
    int pieces_num;

    char info_hash[SHA1_LEN];
    char info_hash_hex[SHA1_LEN*2 + 1];

    int tracker_num;
    char *tracker_url[MAX_TRACKER_NUM];
    
    char *pathname;
    int64 totalsz;
    int isSingleDown;
    struct multi_files mfile;

    int privated;

    char *comment;
    char *creator;
	int create_date;
    struct torrent_task *tsk;
};

enum {
    TRACKER_PROT_UDP = 0,
    TRACKER_PROT_HTTP,
    TRACKER_PROT_HTTPS,
};

struct tracker_prot {
    int prot_type;
	char *host;
	char *port;	
	char *reqpath;
};

enum {
    PEER_STATE_NONE = 0,
    PEER_STATE_CONNECTING,
    PEER_STATE_SEND_HANDSHAKE,
    PEER_STATE_EXCHANGE_BITFIELD,
    PEER_STATE_CONNECTD,
};

enum {
    PEER_MSG_ID_HANDSHAKE = -2,
    PEER_MSG_ID_KEEPALIVE = -1,
    PEER_MSG_ID_CHOCKED = 0,
    PEER_MSG_ID_UNCHOCKED = 1,
    PEER_MSG_ID_INSTRESTED = 2,
    PEER_MSG_ID_NOTINSTRESTED = 3,
    PEER_MSG_ID_HAVE = 4,
    PEER_MSG_ID_BITFIELD = 5,
    PEER_MSG_ID_REQUEST = 6,
    PEER_MSG_ID_PIECE = 7,
    PEER_MSG_ID_CANCEL = 8,
    PEER_MSG_ID_PORT = 9,
    PEER_MSG_ID_INVALID,
};

struct pieces {
    int idx;
    struct pieces *next;
};

struct bitfield {
    char *bitmap;
    int nbyte, npieces;
    int piecesz;
    int64 totalsz;
    struct pieces *pieces_list;
};

struct slice {
    int idx, offset;
    int slicesz, downsz;
    char *data;
    struct slice *next;
};

struct peer_rcv_msg {
    int rcvlen;
    char *rcvbuf;

    int data_transfering;

    int nreq_list;
    struct slice **req_tail;
    struct slice *req_list;

    struct slice **downed_tail;
    struct slice *downed_list;

    struct slice *wait_list;
};

struct peer_send_msg {
    int pieceidx,piecesz;
    char *piecedata;
    int sliceoffset;
    struct slice *req_list; 
    struct slice **req_tail; 
};

struct tracker;
struct ip_addrinfo;
struct peer {
    int isused;
    int state;
    int sockid;
    int tmrfd;
    int heartbeat;
    int am_unchoking;
    int am_interested;
    int peer_unchoking;
    int peer_interested;
    char strfaddr[32];
    char peerid[PEER_ID_LEN];
    struct peer_rcv_msg pm;
    struct peer_send_msg psm;
    struct bitfield bf;
    struct torrent_task *tsk;
    struct peer_addrinfo *ipaddr;
    struct pieces *having_pieces;
};

enum {
    TRACKER_STATE_NONE = 0,

    TRACKER_STATE_CONNECTING,
    TRACKER_STATE_SENDING_REQ,
    TRACKER_STATE_WAITING_RSP,

    TRACKER_STATE_UDP_CONNECT_REQ,
    TRACKER_STATE_UDP_CONNECT_RSP,
    TRACKER_STATE_UDP_ANNOUNCE_REQ,
    TRACKER_STATE_UDP_ANNOUNCE_RSP,
    TRACKER_STATE_UDP_SCRAPE_REQ,
    TRACKER_STATE_UDP_SCRAPE_RSP,
};

struct tracker {
    uint16 port;
    int ip, annouce_time;
    int announce_cnt;
    int sockid, tmrfd, state;
    int transaction_id, connect_cnt;
    int64 conn_id;
    struct tracker_prot tp;
    struct torrent_task *tsk;
    struct tracker *next;
};

enum {
    TASK_STATE_NONE = 0,
    TASK_STATE_START,
    TASK_STATE_STOPED,
    TASK_STATE_COMPLETE,
};

struct peer_addrinfo {
    int client, ip;
    uint16 port;
    int64 downsz, uploadsz;
    int next_connect_time;
    struct peer_addrinfo *next;
};

struct peer_addrinfo_head {
    struct peer_addrinfo *head;
    struct peer_addrinfo **tail;
};

enum {
    PEER_TYPE_ACTIVE_SUPER = 0,
    PEER_TYPE_ACTIVE_NORMAL,
    PEER_TYPE_ACTIVE_NONE,
    PEER_TYPE_ACTIVE_NUM,
};

struct torrent_task {
    int epfd;
    int listenfd, tmrfd;
    uint16 listen_port;
    int task_state;
    int64 down_size;
    int leftpieces;
    struct bitfield bf;
    struct torrent_file tor;

    struct pieces *havelist;

    int npeer;
    struct peer pr[MAX_PEER_NUM];
    struct peer_addrinfo_head pr_list[PEER_TYPE_ACTIVE_NUM];

    struct tracker *tr_active_list;
    struct tracker *tr_inactive_list;
    struct tracker **tr_inactive_list_tail;
};

struct torrent_mgr {
    int tmrfd; 
    int ntask;
    struct torrent_task *tsklist;
};

extern char peer_id[];

#ifdef __cplusplus
extern "C" }
#endif

#endif

/* end of file*/
