#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include "btype.h"

struct offset {
    char *begin;
    char *end;
};

#define ARRAY_ENLARGE_STEP (8)

/* previous declaration */
static int parser_dict(struct offset *offsz, struct benc_type *bt);
static int parser_list(struct offset *offsz, struct benc_type *bt);
static int parser_int(struct offset *offsz, struct benc_type *bt);
static int parser_string(struct offset *offsz, struct benc_type *bt);
static int parser_key(struct offset *offsz, struct benc_type *bt);
static int parser_value(struct offset *offsz, struct benc_type *bt);

static int
enlarge_array(struct benc_type *bt)
{
    char *mem = (char *)bt->val.list.vals;
    int nlist = bt->val.list.nlist;
    int alloced = bt->val.list.alloced;
    int k = (bt->type == BENC_TYPE_DICT ? 2 : 1);

    if(nlist + k > alloced) {
        mem = realloc(mem, sizeof(*bt) * (nlist + ARRAY_ENLARGE_STEP));
        if(!mem) {
            fprintf(stderr, "out of memory!\n");
            return -1;
        }
        memset(mem + nlist * sizeof(*bt), 0, sizeof(*bt)*ARRAY_ENLARGE_STEP);
        bt->val.list.vals = (struct benc_type *)mem;
        bt->val.list.alloced = nlist+ARRAY_ENLARGE_STEP;
    }
    return 0;
}

static int
parser_int(struct offset *offsz, struct benc_type *bt)
{
//    fprintf(stderr, "%s[%x:%x]\n", __func__, offsz->begin, offsz->end);

    if(offsz->begin >= offsz->end || offsz->begin[0] != 'i') {
        return -1;
    }

    offsz->begin++;

    if(offsz->begin >= offsz->end || !isdigit(offsz->begin[0])) {
        return -1;
    }

    char *ptr;
    errno = 0;
    int digit = (int)strtol(offsz->begin, &ptr, 10);
    if(errno || ptr[0] != 'e') {
        return -1;
    }

    offsz->begin = ptr+1;

    bt->type = BENC_TYPE_INT;
    bt->val.i = digit;

    return 0;
}

static int
parser_string(struct offset *offsz, struct benc_type *bt)
{
//    fprintf(stderr, "%s[%x:%x]\n", __func__, offsz->begin, offsz->end);

    if(offsz->begin >= offsz->end) {
        return -1;
    }
    
    if(!isdigit(offsz->begin[0])) {
        return -1;
    }

    char *ptr;
    errno = 0;
    int strlen = (int)strtol(offsz->begin, &ptr, 10);
    if(errno || strlen < 0 || ptr[0] != ':') { // strlen can be zero
        return -1;
    }

    ptr++;
    if(ptr+strlen >= offsz->end) {
        return -1;
    }

    char *str = malloc(strlen+1);
    if(!str) {
        fprintf(stderr, "out of memory!\n");
        return -1;
    }
    str[strlen] = '\0';
    memcpy(str, ptr, strlen);

    offsz->begin = ptr+strlen;

    bt->type = BENC_TYPE_STRING;
    bt->val.str.s = str;
    bt->val.str.len = strlen;

    return 0;
}

static int
parser_key(struct offset *offsz, struct benc_type *bt)
{
    return parser_string(offsz, bt);
}

static int
parser_value(struct offset *offsz, struct benc_type *bt)
{  
    if(offsz->begin >= offsz->end) {
        return -1;
    }

    switch(offsz->begin[0]) {
        case 'd':
            return parser_dict(offsz, bt);
        case 'l':
            return parser_list(offsz, bt);
        case 'i':
            return parser_int(offsz, bt); 
        default:
            return parser_string(offsz, bt);
    }

    return -1;
}

static int
parser_list(struct offset *offsz, struct benc_type *bt)
{
//    fprintf(stderr, "%s[%x:%x]\n", __func__, offsz->begin, offsz->end);

    if(offsz->begin >= offsz->end || offsz->begin[0] != 'l') {
        return -1;
    }

    offsz->begin++; //skip 'l'

    bt->type = BENC_TYPE_LIST;

    while(offsz->begin < offsz->end && offsz->begin[0] != 'e') {
        if(enlarge_array(bt)) {
            return -1;
        }
        if(parser_value(offsz, bt->val.list.vals + bt->val.list.nlist)) {
            return -1;
        }
        bt->val.list.nlist ++;
    }

    if(offsz->begin < offsz->end && offsz->begin[0] == 'e') {
        offsz->begin++; // skip 'e'
        return 0;
    }

    return -1;
}

static int
parser_dict(struct offset *offsz, struct benc_type *bt)
{
//    fprintf(stderr, "%s[%x:%x]\n", __func__, offsz->begin, offsz->end);

    if(offsz->begin >= offsz->end || offsz->begin[0] != 'd') {
        return -1;
    }

    offsz->begin++; //skip 'd'

    bt->type = BENC_TYPE_DICT;

    while(offsz->begin < offsz->end && offsz->begin[0] != 'e') {
        if(enlarge_array(bt)) {
            return -1;
        }
        if(parser_key(offsz, bt->val.list.vals + bt->val.list.nlist)) {
            return -1;
        }
        if(parser_value(offsz, bt->val.list.vals + bt->val.list.nlist+1)) {
            return -1;
        }
        bt->val.list.nlist += 2;
    }

    if(offsz->begin < offsz->end && offsz->begin[0] == 'e') {
        offsz->begin++; // skip 'e'
        return 0;
    }

    return -1;
}

static void dump_benc_type(struct benc_type *bt)
{
    int i;
    switch(bt->type) {
        case BENC_TYPE_INT:
              fprintf(stderr, "int:%ld\n", bt->val.i);
              break;
        case BENC_TYPE_STRING:
              fprintf(stderr, "str:");
              for(i = 0; i < bt->val.str.len; i++) {
                  fprintf(stderr, "%c", bt->val.str.s[i]);
                  if(i > 64) break;
              }
              fprintf(stderr, "\n");
              break;
        case BENC_TYPE_LIST:
              fprintf(stderr, "list:\n");
              for(i = 0; i < bt->val.list.nlist; i++) {
                  dump_benc_type(bt->val.list.vals+i);
              }
              fprintf(stderr, ":list\n");
              break;
        case BENC_TYPE_DICT:
              fprintf(stderr, "dict:\n");
              for(i = 0; i < bt->val.list.nlist; i+=2) {
                  dump_benc_type(bt->val.list.vals+i);
                  dump_benc_type(bt->val.list.vals+i+1);
              }
              fprintf(stderr, ":dict\n");
              break;
        case BENC_TYPE_BOOL:
              break;
        case BENC_TYPE_DOUBLE:
              break;
        default:
              break;
    }
}

static int
do_torfile_parser(char *bufbegin, size_t filesz, struct torrent_file *tor)
{
    struct offset offsz;
    
    offsz.begin = bufbegin;
    offsz.end = bufbegin + filesz;

    memset(&tor->bt, 0, sizeof(struct benc_type));

    if(parser_dict(&offsz, &tor->bt)) {
        return -1;
    }

//    dump_benc_type(&tor->bt);

    return 0;
}

int
torrent_file_parser(char *torfile, struct torrent_file *tor)
{
    if(!(tor->torfile = strdup(torfile))) {
        fprintf(stderr, "strdup fained!\n");
        return -1;
    }

    int fd = open(torfile, O_RDONLY);
    if(fd < 1) {
        fprintf(stderr, "open %s failed[%s]\n", torfile, strerror(errno));
        return -1;
    }

    struct stat st;
    if(fstat(fd, &st) == -1) {
        fprintf(stderr, "stat %d failed[%s]\n", fd, strerror(errno));
        close(fd);
        return -1;
    }
    
    char *memaddr = mmap(NULL, st.st_size,  PROT_READ, MAP_PRIVATE, fd, 0);
    if(!memaddr) {
        fprintf(stderr, "mmap %d failed[%s]\n", fd, strerror(errno));
        close(fd);
        return -1;
    }

    int res = do_torfile_parser(memaddr, st.st_size, tor);

    close(fd);
    munmap(memaddr, st.st_size);

    return res;
}

