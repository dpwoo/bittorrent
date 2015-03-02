#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "btype.h"
#include "bitfield.h"
#include "log.h"

#define SLICE_SZ (16*1024)

static int bitfield_piece_insert_by_sort(struct pieces **list, struct pieces *node);
static int bitfield_piece_remove(struct pieces **p, int idx);
static int bitfield_piece_find(struct pieces *p, int idx);
static int bitfield_build_slice_list(struct bitfield *bf, struct peer_msg *pm, int idx);

int
bitfield_create(struct bitfield *bf, int pieces_num, int piece_sz, int totalsz)
{
    if(!bf || pieces_num <= 0 || piece_sz <= 0 || totalsz <= 0) {
        LOG_ERROR("invalid param!\n");
        return -1;
    }

    bf->npieces = pieces_num;
    bf->nbyte = (pieces_num + 7) / 8;
    bf->totalsz = totalsz;
    bf->piecesz = piece_sz;

    bf->bitmap = calloc(1, bf->nbyte);
    if(!bf->bitmap) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    return 0;
}

int
bitfield_dup(struct bitfield *bf, char *bitmap, int nbyte)
{
    if(!bf || !bf->bitmap || !bitmap || bf->nbyte != nbyte) {
        LOG_ERROR("invalid param!\n");
        return -1;
    }

    memcpy(bf->bitmap, bitmap, nbyte);

    return 0;
}

int
bitfield_intrested(struct bitfield *local, struct bitfield *peer)
{
    if(!local || !peer || !local->bitmap || !peer->bitmap
                    || local->npieces != peer->npieces
                    || local->nbyte != peer->nbyte) {
        LOG_ERROR("invalid param!\n");
        return 0;
    }

    peer->pieces_list = NULL;
    struct pieces *tmp, **p = &peer->pieces_list;

    int i, k, idx;
    for(i = 0; i < local->nbyte; i++) {
        unsigned char c1 = (unsigned char)local->bitmap[i];
        unsigned char c2 = (unsigned char)peer->bitmap[i];
        unsigned char c3  = (c1|c2) ^ c1;

        for(k = 7; c3 && k >= 0; k--) {
            if(c3 & (1<<k)) {
                idx = (i*8)+(7-k);
                if(!(tmp = calloc(1, sizeof(*tmp)))) {
                    goto MEM_FAILED;
                }
                tmp->idx = idx;
                *p = tmp;
                p = &(*p)->next;
            }
        }
    }

    return peer->pieces_list ? 1 : 0;

MEM_FAILED:
    LOG_ERROR("out of mmemory!\n");
    while(peer->pieces_list) {
        tmp = peer->pieces_list;
        peer->pieces_list = tmp->next;
        free(tmp);
    }
    return 0;
}

int
bitfield_peer_garbage_piece(struct bitfield *local, int idx)
{
    if(idx < 0 || idx >= local->npieces) {
        return -1;
    }

    bitfield_piece_remove(&local->pieces_list, idx);

    return 0;
}

int
bitfield_local_have(struct bitfield *local, int idx)
{
    if(idx < 0 || idx >= local->npieces) {
        return -1;
    }

    int pidx = idx >> 3; /* idx/8 */
    int bidx = idx & 7;  /* idx%8 */
    unsigned char *byte = (unsigned char *)&local->bitmap[pidx];

    if( ((*byte) & (1 << (7-bidx))) || bitfield_piece_remove(&local->pieces_list, idx)) {
        LOG_ERROR("local have piece[%d] error!\n", idx);
        return -1;
    }

    *byte |= (1 << (7-bidx));

    return 0;
}

int
bitfield_peer_have(struct bitfield *local, struct bitfield *peer, int idx)
{
    if(idx < 0 || idx > local->npieces) {
        return -1;
    }

    int pidx = idx >> 3; /* idx/8 */
    int bidx = idx & 7;  /* idx%8 */
    unsigned char byte = local->bitmap[pidx];

    if( byte & (1 << (7-bidx)) ) {
        return -1; /* we already have this piece */
    }

    struct pieces *p = calloc(1, sizeof(*p));
    if(!p) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }
    p->idx = idx;

    bitfield_piece_insert_by_sort(&peer->pieces_list, p);

    return 0;
}

static int
bitfield_piece_insert_by_sort(struct pieces **list, struct pieces *node)
{
    for(; *list && (*list)->idx < node->idx; list = &(*list)->next) {
        /* nothing */ 
    }
    node->next = *list;
    *list = node;
    return 0;
}

static int
bitfield_piece_remove(struct pieces **p, int idx)
{
    for(; *p; p = &(*p)->next) {
        if((*p)->idx == idx) {
            struct pieces *tmp = *p;
            *p = tmp->next;
            free(tmp);
            return 0;
        }
    }
    return -1;
}

static int
bitfield_piece_find(struct pieces *p, int idx)
{
    for(; p; p = p->next) {
        if(p->idx == idx) {
            return 0;
        }
    }
    return -1;
}

static int
bitfield_build_slice_list(struct bitfield *bf, struct peer_msg *pm, int idx)
{
    int nslice, last_piecesz = 0, last_slicesz = 0;

    nslice = (bf->piecesz + (SLICE_SZ-1)) /  SLICE_SZ;
    last_piecesz = bf->totalsz % bf->piecesz;

    if(idx == bf->npieces-1 && last_piecesz != 0) {
        nslice = (last_piecesz + (SLICE_SZ-1)) / SLICE_SZ;
        last_slicesz = last_piecesz % SLICE_SZ;
    }

    pm->downed_tail = &pm->downed_list;
    pm->downed_list = NULL;

    pm->req_tail = &pm->req_list;
    pm->req_list = NULL;

    pm->wait_list = calloc(nslice, sizeof(struct slice)); 
    if(!pm->wait_list) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    int i;
    for(i = 0; i < nslice; i++) {
        pm->wait_list[i].idx = idx;
        pm->wait_list[i].slicesz = SLICE_SZ;
        pm->wait_list[i].offset = i * SLICE_SZ;
        pm->wait_list[i].next = (i != nslice-1) ? pm->wait_list + i + 1 : NULL;
    }

    if(last_slicesz) {
        pm->wait_list[nslice-1].slicesz = last_slicesz;
    }

    return 0;
}

int
bitfield_get_request_piece(struct bitfield *local, struct bitfield *peer, struct peer_msg *pm)
{
    if(!local || !peer || !pm) {
        LOG_ERROR("invalid param!\n");
        return -1;
    }
     
    struct pieces *tmp, **p = &peer->pieces_list;
    while(*p) {
        int idx = (*p)->idx;
        int pidx = idx >> 3; /* idx/8 */
        int bidx = idx & 7;  /* idx%8 */
        unsigned char byte = local->bitmap[pidx];

        if( (byte & (1 << (7-bidx))) ) { /* local alread have */
            tmp = *p;
            *p = (*p)->next; /* remove this one */
            free(tmp);
            continue;
        } else if(!bitfield_piece_find(local->pieces_list, idx)){ /* downloading */
            p = &(*p)->next; /* skip this one */
            continue;
        } else {
            tmp = *p; 
            *p = (*p)->next; /* remove this one */
            tmp->next = local->pieces_list;
            local->pieces_list = tmp;

            bitfield_build_slice_list(local, pm, tmp->idx);

            return 0;
        }
    }

    /* TODO select a downloading piece */

    return -1;
}

