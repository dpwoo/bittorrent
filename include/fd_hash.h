#ifndef FD_HASH_H
#define FD_HASH_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SLOT_TYPE_PEER = 0,
    SLOT_TYPE_TRACKER,
    SLOT_TYPE_TIMER,
    SLOT_TYPE_END,
};

int fd_hash_add(int fd, void *usrctx);

void* fd_hash_find(int fd);

int fd_hash_del(int fd);

#ifdef __cplusplus
extern "C" }
#endif

#endif
