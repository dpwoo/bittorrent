/*
 *  file btype.h
 *  author dpwoo
 *  date  2015-01-17 
 */

#ifndef BTYPE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define DOWN_TYPE_TYPE_SINGLE 1
#define DOWN_TYPE_TYPE_MANY 2

#define SHA1_LEN 20
#define MAX_TRACKER_NUM 32

enum {
    BENC_TYPE_NONE = 0,
    BENC_TYPE_BOOL ,
    BENC_TYPE_INT ,
    BENC_TYPE_DOUBLE ,
    BENC_TYPE_STRING ,
    BENC_TYPE_LIST ,
    BENC_TYPE_DICT ,
};

struct benc_type {
    int type;
    union{
        int b;
        long i;
        double d;
        struct {
            int len;
            char *s;
        }str;
        struct {// for list and dictionary
            int nlist, alloced;
            struct benc_type *vals;
        }list;
   }val;
};

struct single_file {
    char *pathname;
    size_t file_size;
};

/* if files_num equal 1(means single file for down),
 * then files field not used, and
 * use totalsz, pathname in torrent_file
 */
struct many_files {
    size_t files_num;
    struct single_file *files;
};

struct torrent_file {
    char *torfile;
    struct benc_type bt;

    size_t piece_len;
    char *pieces;
    size_t pieces_num;

    char *announce;
    char *tracker_url[MAX_TRACKER_NUM];
    size_t tracker_num;

    char *pathname;
    int totalsz;
    int isSingleDown;
    struct many_files mfile;

    char *comment;
    char *torrmaker;
    char *creator;
	int create_date;
};

struct peer {
    char *ip;
    unsigned short port;
    int peer_state;
    size_t snd_size;
    size_t rcv_size;
};

struct torrent_task {
    struct torrent_file torrent;
    unsigned short port;
    int task_state;
    size_t down_size;
};

#ifdef __cplusplus
extern "C" }
#endif
#endif

/* end of file*/
