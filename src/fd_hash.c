#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fd_hash.h"
#include "log.h"

struct hash_slot {
    int fd;
    void *usrctx;
    struct hash_slot *next;
};

#define BUKET_NUM (1024-1)
struct _fd_hash {
    int used;
    struct hash_slot *slots[BUKET_NUM];
};

static struct _fd_hash fd_hash;

int
fd_hash_add(int fd, void *usrctx)
{
    if(fd < 0) {
        return -1;
    }

    struct hash_slot *hs;
    hs = malloc(sizeof(*hs));
    if(!hs) {
        LOG_ERROR("%s:out of memory!\n", __func__);
        return -1;
    }

    hs->fd = fd;
    hs->usrctx = usrctx;

    hs->next = fd_hash.slots[fd % BUKET_NUM];
    fd_hash.slots[fd % BUKET_NUM] = hs;
    fd_hash.used++;

    return 0;
}

void*
fd_hash_find(int fd)
{
    if(fd < 0) {
        return NULL;
    }

    struct hash_slot *hs;
    hs = fd_hash.slots[fd % BUKET_NUM];
    while(hs) {
        if(hs->fd == fd) {
           return hs->usrctx; 
        }
        hs = hs->next;
    }

    return NULL;
}

int
fd_hash_del(int fd)
{
    if(fd < 0 || fd_hash.used <= 0) {
        return -1;
    }

    struct hash_slot *del, **hs;
    hs = &fd_hash.slots[fd % BUKET_NUM];
    while(*hs) {
        if((*hs)->fd == fd) {
            del = (*hs);
            *hs = (*hs)->next;
            free(del);
            fd_hash.used--;
            return 0; 
        }
        hs = &(*hs)->next;
    }

    return -1;
}

