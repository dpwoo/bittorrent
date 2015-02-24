#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "btype.h"
#include "bitfield.h"
#include "log.h"

#define SLICE_SZ (16*1024)

static void 
bitfield_dump(struct bitfield *bf)
{
    struct pieces *p;
    for(p = bf->pieces; p; p = p->next) {
        LOG_DEBUG("piece[%d]:\n", (p-bf->pieces));
        struct slice *sl;
        for(sl = p->slice; sl; sl = sl->next) {
            LOG_DEBUG("slice[%d]:[%d,%d]\n",sl-p->slice, sl->offset, sl->size);
        }
    }
}

static int
bitfield_local_init(struct bitfield *bf, int pieces_num, int piece_sz, int totalsz)
{
    int maxslice = (piece_sz + (SLICE_SZ -1)) / (SLICE_SZ);

    int i, j;
    for(j = 0; j < 2; j++) {
        bf->slice[j] =  calloc(maxslice, sizeof(struct slice));
        if(!bf->slice[j]) {
            LOG_ERROR("out of memory!\n");
            return -1;
        }

        for(i = 0; i < maxslice; i++) {       
            bf->slice[j][i].offset = i*SLICE_SZ;
            bf->slice[j][i].size = SLICE_SZ;
            bf->slice[j][i].next = bf->slice[j] + i + 1;
        }
        bf->slice[j][maxslice-1].next = NULL;
    }

    for(i = 0; i < pieces_num; i++) {
        bf->pieces[i].slice = bf->slice[0];
        bf->pieces[i].pre = bf->pieces + i - 1;
        bf->pieces[i].next = bf->pieces + i + 1;
    }
    bf->pieces[0].pre = NULL;
    bf->pieces[pieces_num-1].next = NULL;
    bf->pieces[pieces_num-1].slice = bf->slice[1];

    int last_piece = totalsz % piece_sz;
    if(last_piece) {
        int nslice = (last_piece+(SLICE_SZ-1)) / SLICE_SZ;
        int last_slice = last_piece % SLICE_SZ;
        bf->slice[1][nslice-1].next = NULL;
        if(last_slice) {
            bf->slice[1][nslice-1].size = last_slice;
        }
    }

    return 0;
}

int
bitfield_create(struct bitfield *bf, int pieces_num, int piece_sz, int totalsz, int local)
{
    if(!bf || pieces_num <= 0 || (local && piece_sz <= 0 && totalsz <= 0)) {
        LOG_ERROR("invalid param!\n");
        return -1;
    }

    bf->npieces = pieces_num;
    bf->nbyte = (pieces_num + 7) / 8;

    bf->bitmap = calloc(1, bf->nbyte);
    if(!bf->bitmap) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    bf->pieces_slot = bf->pieces = calloc(pieces_num, sizeof(struct pieces));
    if(!bf->pieces_slot) {
        LOG_ERROR("out of memory!\n");
        goto MEM_FAILED;
    }

    if(local && bitfield_local_init(bf, pieces_num, piece_sz, totalsz)) {
        goto MEM_FAILED;
    }

#if 0
    bitfield_dump(bf);
#endif

    return 0;

MEM_FAILED:
    free(bf->bitmap);
    bf->bitmap = NULL;

    free(bf->pieces_slot);
    bf->pieces = NULL;
    bf->pieces_slot = NULL;

    free(bf->slice[0]);
    free(bf->slice[1]);
    bf->slice[0] = NULL;
    bf->slice[1] = NULL;

    return -1;
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
bitfield_intrested(struct bitfield *me, struct bitfield *peer)
{
    if(!me || !peer || !me->bitmap || !peer->bitmap
                    || me->npieces != peer->npieces
                    || me->nbyte != peer->nbyte) {
        LOG_ERROR("invalid param!\n");
        return 0;
    }

    peer->pieces = NULL;
    struct pieces **p = &peer->pieces;

    int i, k, idx;
    for(i = 0; i < me->nbyte; i++) {
        unsigned char c1 = (unsigned char)me->bitmap[i];
        unsigned char c2 = (unsigned char)peer->bitmap[i];
        unsigned char c3  = (c1|c2) ^ c1;

        for(k = 7; c3 && k >= 0; k--) {
            if(c3 & (1<<k)) {
                idx = (i*8)+(7-k);
                *p = &peer->pieces_slot[idx];
                p = &(*p)->next;
            }
        }
    }

    return peer->pieces ? 1 : 0;
}

int
bitfield_local_have(struct bitfield *me, int idx)
{
    if(idx < 0 || idx >= me->npieces) {
        return -1;
    }

    int pidx = idx >> 3; /* idx/8 */
    int bidx = idx & 3;  /* idx%8 */
    unsigned char *byte = (unsigned char *)&me->bitmap[pidx];
    (*byte) |= 1 << bidx;

    return 0;
}

int
bitfield_peer_have(struct bitfield *me, struct bitfield *peer, int idx)
{
    if(idx < 0 || idx > me->npieces) {
        return -1;
    }

    int pidx = idx >> 3; /* idx/8 */
    int bidx = idx & 3;  /* idx%8 */
    unsigned char byte = me->bitmap[pidx];

    if(byte & (1 << bidx)) {
        return 0; /* we already have this piece */
    }

    return 0;
}

int
bitfield_get_request(struct bitfield *me, struct bitfield *peer, int *idx, int *offset, int *size)
{
    if(!me || !peer || !idx || !offset || !size) {
        LOG_ERROR("invalid param!\n");
        return -1;
    }
     
    for(; peer->pieces; peer->pieces = peer->pieces->next) {
        int idx_piece;
        
        idx_piece = peer->pieces - peer->pieces_slot;
        if(me->pieces_slot[idx_piece].slice) {
            struct slice *sl;
            
            sl = me->pieces_slot[idx_piece].slice;
            me->pieces_slot[idx_piece].slice = sl->next;
            *idx = idx_piece;
            *offset = sl->offset;
            *size = sl->size;

            return 0;
        }
    }

    return -1;
}

