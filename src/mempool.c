#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <error.h>
#include "mempool.h"
#include "log.h"

#ifdef USED_MEMPOOL

#define MAGIC (0xdeadbeaf)
#define HUGE_SIZE (0x7fffffff)

static int mempool_do_init(struct mempool *mp);
static int mempool_do_uninit(struct mempool *mp);
static int mem_get_pool_type(struct mempool *mp, int size);
static void *mem_do_alloc(struct mempool_unit *mpu, int size, const char *file, int line);
static int mem_do_free(struct mempool_unit *mpu, struct mem_unit *mu, const char *file, int line);

static int mem_config[MEM_POOL_TYPE_NUM] =
       {16, 64, 256, 512, 1024, 2048, 4096, 16*1024, 512*1024, HUGE_SIZE};

struct mempool global_mpool;

static int
mempool_do_init(struct mempool *mp)
{
    int i;
    for(i = 0; i < MEM_POOL_TYPE_NUM; i++) {
        mp->pool[i].pool[MEM_POOL_FREE] = NULL;
        mp->pool[i].pool[MEM_POOL_USED] = NULL;
        mp->pool[i].unitsz = mem_config[i];
    }

    mp->magic = MAGIC;

    return 0;
}

static int
mempool_do_uninit(struct mempool *mp)
{
    int i, k;
    for(i = 0; i < MEM_POOL_TYPE_NUM; i++) {
        struct mem_unit *tmp, *mu;
        for(k = 0; k < MEM_POOL_NUM; k++) {
            for(mu = mp->pool[i].pool[k]; mu; ) {
                tmp = mu;
                mu = mu->next;
                free(tmp);
            }
        }
    }

    mp->magic = 0;

    return 0;
}

struct mempool*
mempool_init(void)
{
    struct mempool *mp;
    mp = calloc(1, sizeof(*mp));
    if(!mp) {
        LOG_ERROR("out of memory!\n");
        return NULL;
    }
   
    mempool_do_init(mp);

    return mp;
}

int
mempool_uninit(struct mempool *mp)
{
    if(!mp || mp->magic != MAGIC) {
        return -1;
    }
    mempool_do_uninit(mp);
    free(mp);

    return 0;
}

int
mempool_init_global(void)
{
    return mempool_do_init(&global_mpool);
}

int
mempool_uninit_global(void)
{
    return mempool_do_uninit(&global_mpool);
}

static int
mem_get_pool_type(struct mempool *mp, int size)
{
    int i;
    for(i = 0; i < MEM_POOL_TYPE_NUM && size > mem_config[i]; i++) {
        /* nothing */
    }
    return i;
}

static void* 
mem_do_alloc(struct mempool_unit *mpu, int size, const char *file, int line)
{
    struct mem_unit *mu = NULL;

    if(mpu->pool[MEM_POOL_FREE]) {
        mu = mpu->pool[MEM_POOL_FREE];
        mpu->pool[MEM_POOL_FREE] = mu->next;
    }

    int mallocsz = mpu->unitsz == HUGE_SIZE ? size : mpu->unitsz;

    if(!mu && !(mu = malloc(sizeof(*mu) + mallocsz))) {
        LOG_ERROR("out of memory[%d,%d,%s]!\n", mallocsz, line, file);
        return NULL;
    }

    mu->magic = MAGIC;
    mu->buffer = (char*)(mu + 1);
    mu->size = size;
    mu->file = file;
    mu->line = line;
    mu->time = time(NULL);

    mu->next = mpu->pool[MEM_POOL_USED];
    mpu->pool[MEM_POOL_USED] = mu;

    return mu->buffer;
}

static int 
mem_do_free(struct mempool_unit *mpu, struct mem_unit *mu, const char *file, int line)
{
    struct mem_unit **iter;

    for(iter = &mpu->pool[MEM_POOL_USED]; *iter && mu != *iter; iter = &(*iter)->next) {
        /* nothing */
    }

    if(*iter) {
        *iter = (*iter)->next;
        if(mpu->unitsz == HUGE_SIZE) {
            free(mu);
            return 0;
        }
        mu->next = mpu->pool[MEM_POOL_FREE];
        mpu->pool[MEM_POOL_FREE] = mu;
        return 0;
    }

    LOG_ERROR("mem free failed: can't find mu[%X][%d,%s]!\n", mu, line, file);

    return -1;
}

void*
mem_malloc(struct mempool *mp, int size, const char *file, int line)
{
    if(!mp || mp->magic != MAGIC || size <= 0) {
        LOG_ERROR("invalid param[%d][%d,%s]!\n", size, line, file);
        return NULL;
    }

    int idx = mem_get_pool_type(mp, size);

    return mem_do_alloc(&mp->pool[idx], size, file, line);
}

void*
mem_calloc(struct mempool *mp, int nmemb, int size, const char *file, int line)
{
    if(!mp || mp->magic != MAGIC || nmemb <= 0 || size <= 0) {
        LOG_ERROR("invalid param[%d, %d][%d,%s]!\n", nmemb, size, line, file);
        return NULL;
    }

    int totalsz = nmemb *size;
    int idx = mem_get_pool_type(mp, totalsz);

    char *buf = mem_do_alloc(&mp->pool[idx], totalsz, file, line);
    if(buf) {
        memset(buf, 0, totalsz);
    }

    return buf ? buf : NULL;
}

void*
mem_realloc(struct mempool *mp, void *addr, int size, const char *file, int line)
{
    if(!mp || (!addr && !size)) {
        LOG_ERROR("invalid param[%d,%s]!\n", line, file);
        return NULL;
    }

    if(addr && !size) {
        mem_free(mp, addr, file, line);
        return NULL;
    }

    if(!addr && size) {
        return mem_malloc(mp, size, file, line);
    }

    struct mem_unit *mu;
    mu = (struct mem_unit *)((char *)addr - sizeof(*mu));
    if(mu->magic != MAGIC) {
        LOG_ERROR("mem realloc failed[%d,%s]!\n", line, file);
        return NULL;
    }

    int idx = mem_get_pool_type(mp, mu->size);
    if(mp->pool[idx].unitsz != HUGE_SIZE && mp->pool[idx].unitsz >= size) {
        mu->size = size;
        mu->file = file;
        mu->line = line;
        mu->time = time(NULL);
        return mu->buffer;
    }

    int new_idx = mem_get_pool_type(mp, size);
    char *newbuf = mem_do_alloc(&mp->pool[new_idx], size, file, line);
    if(!newbuf) {
        return NULL;
    }
    
    memcpy(newbuf, mu->buffer, mu->size);

    mem_do_free(&mp->pool[idx], mu, file, line);

    return newbuf;
}

int
mem_free(struct mempool *mp, void *addr, const char *file, int line)
{
    if(!mp || mp->magic != MAGIC) {
        LOG_ERROR("invalid param[%X][%d,%s]!\n", addr, line, file);
        return -1;
    }

    if(!addr) {
        return 0;
    }

    struct mem_unit *mu;
    mu = (struct mem_unit *)((char *)addr - sizeof(*mu));
    if(mu->magic != MAGIC) {
        LOG_ERROR("mem free failed[%d,%s]!\n", line, file);
        return -1;
    }

    int idx = mem_get_pool_type(mp, mu->size);

    return mem_do_free(&mp->pool[idx], mu, file, line);
}

char*
mem_strdup(struct mempool *mp, const char *s, const char *file, int line)
{
    if(!s) {
        LOG_ERROR("invalid param!\n");
        return NULL;
    }
    
    int totalsz = strlen(s)+1;
    int idx = mem_get_pool_type(mp, totalsz);

    char *buf = mem_do_alloc(&mp->pool[idx], totalsz, file, line);
    if(buf) {
        memcpy(buf, s, totalsz);
    }

    return buf ? buf : NULL;
}

void
mem_dump(struct mempool *mp)
{
    if(!mp || mp->magic != MAGIC) {
        LOG_ERROR("invalid param!\n");
        return ;
    }

    int i, now = time(NULL);

    fprintf(stderr, "size   second    line    file\n");
    for(i = 0; i < MEM_POOL_TYPE_NUM; i++) {
        struct mem_unit *mu = mp->pool[i].pool[MEM_POOL_USED];
        for(; mu; mu = mu->next) {
            fprintf(stderr, "%08d %06d %04d %s\n",
                mu->size, now - mu->time, mu->line,
                strrchr(mu->file, '/') ? strrchr(mu->file, '/') : mu->file);
        }
    }
}

#endif
