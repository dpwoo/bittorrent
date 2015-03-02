#ifndef BITFIELD_H
#define BITFIELD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "type.h"

struct bitfield;
struct peer_msg;

int bitfield_create(struct bitfield *bf, int pieces_num, int piece_sz, int64 totalsz);

int bitfield_dup(struct bitfield *bf, char *bitmap, int nbyte);

int bitfield_intrested(struct bitfield *local, struct bitfield *peer);

int bitfield_local_have(struct bitfield *local, int idx);

int bitfield_peer_garbage_piece(struct bitfield *local, int idx);

int bitfield_peer_have(struct bitfield *local, struct bitfield *peer, int idx);

int bitfield_get_request_piece(struct bitfield *local, struct bitfield *peer, struct peer_msg *pm);

#ifdef __cplusplus
extern "C" }
#endif

#endif
