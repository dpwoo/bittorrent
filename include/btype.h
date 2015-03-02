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
        int b;
        int64 i;
        double d;
        struct {
            int len;
            char *s;
        }str;
        struct {// for list and dictionary
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

struct peer_msg {
    int rcvlen;
    char rcvbuf[8*1024];

    int data_transfering;

    int nreq_list;
    
    struct slice **req_tail;
    struct slice *req_list;

    struct slice **downed_tail;
    struct slice *downed_list;

    struct slice *wait_list;
};

struct tracker;
struct peer {
    int isused;
    int state;
    int substate;
    int sockid;
    int tmrfd;
    int ip;
    unsigned short port;
    int am_unchoking;
    int am_interested;
    int peer_unchoking;
    int peer_interested;
    int64 snd_size;
    int64 rcv_size;
    struct peer_msg pm;
    struct tracker *tr;
    struct bitfield bf;
};

enum {
    TRACKER_STATE_NONE = 0,
    TRACKER_STATE_CONNECTING,
    TRACKER_STATE_SENDING_REQ,
    TRACKER_STATE_WAITING_RSP,
};

struct addrinfo;
struct tracker {
    int sockid;
    int tmrfd;
    int state;
    int url_index;
    struct tracker_prot tp;
    struct addrinfo *ai;
    struct torrent_task *tsk;
    int npeer;
    struct peer pr[MAX_PEER_NUM];
};

enum {
    TASK_STATE_NONE = 0,
    TASK_STATE_START,
    TASK_STATE_STOPED,
    TASK_STATE_COMPLETE,
};

struct torrent_task {
    int epfd;
    int listen_port;
    int task_state;
    int64 down_size;
    struct tracker tr;
    struct bitfield bf;
    struct torrent_file tor;
};

struct torrent_mgr {
    
};

#ifdef __cplusplus
extern "C" }
#endif

#endif

/* end of file*/
