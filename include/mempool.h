#ifndef MEMPOOL_H
#define MEMPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#define USED_MEMPOOL

#ifdef USED_MEMPOOL
enum {
    MEM_POOL_TYPE_16B = 0,
    MEM_POOL_TYPE_64B,
    MEM_POOL_TYPE_256B,
    MEM_POOL_TYPE_512B,
    MEM_POOL_TYPE_1024B,
    MEM_POOL_TYPE_2048B,
    MEM_POOL_TYPE_4096B,
    MEM_POOL_TYPE_16K,
    MEM_POOL_TYPE_512K,
    MEM_POOL_TYPE_HUGE,
    MEM_POOL_TYPE_NUM,
};

enum {
    MEM_POOL_FREE = 0,
    MEM_POOL_USED,
    MEM_POOL_NUM,
};

struct mem_unit {
    struct mem_unit *next;
    int time;
    int line;
    const char *file;
    int size;
    int magic;
    char *buffer;
};

struct mempool_unit {
    int unitsz;
    struct mem_unit *pool[MEM_POOL_NUM];
};

struct mempool {
    int magic;
    struct mempool_unit pool[MEM_POOL_TYPE_NUM];
};


extern struct mempool global_mpool;

struct mempool *mempool_init(void);
int mempool_uninit(struct mempool *mp);

int mempool_init_global(void);
int mempool_uninit_global(void);

void *mem_malloc(struct mempool *mp, int size, const char *file, int line);

void *mem_calloc(struct mempool *mp, int nmemb, int size, const char *file, int line);

void *mem_realloc(struct mempool *mp, void *addr, int size, const char *file, int line);

int mem_free(struct mempool *mp, void *addr, const char *file, int line);

void mem_dump(struct mempool *mp);

char *mem_strdup(struct mempool *mp, const char *s, const char *file, int line);

#define MALLOC(mp, size) mem_malloc(mp, size, __FILE__, __LINE__)
#define CALLOC(mp, nmemb, size) mem_calloc(mp, nmemb, size, __FILE__, __LINE__)
#define REALLOC(mp, addr, size) mem_realloc(mp, addr, size, __FILE__, __LINE__)
#define FREE(mp, addr) mem_free(mp, addr, __FILE__, __LINE__)

#define GMALLOC(size) mem_malloc(&global_mpool, size, __FILE__, __LINE__)
#define GCALLOC(nmemb, size) mem_calloc(&global_mpool, nmemb, size, __FILE__, __LINE__)
#define GREALLOC(addr, size) mem_realloc(&global_mpool, addr, size, __FILE__, __LINE__)
#define GFREE(addr) mem_free(&global_mpool, addr, __FILE__, __LINE__)
#define GSTRDUP(s) mem_strdup(&global_mpool, s, __FILE__, __LINE__)

#define MEM_DUMP(mp) \
do { \
    if(mp) \
        mem_dump(mp); \
    else \
        mem_dump(&global_mpool); \
}while(0)

#else

#include <stdlib.h>
#include <string.h>

#define MALLOC(mp, size) malloc(size)
#define CALLOC(mp, nmemb, size) calloc(nmemb, size)
#define REALLOC(mp, addr, size) realloc(addr, size)
#define FREE(mp, addr) free(addr)

#define GMALLOC(size) malloc(size)
#define GCALLOC(nmemb, size) calloc(nmemb, size)
#define GREALLOC(addr, size) realloc(addr, size)
#define GFREE(addr) free(addr)
#define GSTRDUP(str) strdup(str)

#define MEM_DUMP(mp)  \
do { \
 /* nothing */ \
}while(0)

#define mempool_init() (0)
#define mempool_uninit() (0)

#define mempool_init_global() (0)
#define mempool_uninit_global() (0)

#define mem_dump(mp) \
do { \
    /* nothing */ \
}while(0)

#endif

#ifdef __cplusplus
extern "C" }
#endif

#endif

