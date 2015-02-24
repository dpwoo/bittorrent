#ifndef BITFIELD_H
#define BITFIELD_H

#ifdef __cplusplus
extern "C" {
#endif

struct bitfield;

int bitfield_create(struct bitfield *bf, int pieces_num, int piece_sz, int totalsz, int local);

int bitfield_dup(struct bitfield *bf, char *bitmap, int nbyte);

int bitfield_intrested(struct bitfield *me, struct bitfield *peer);

int bitfield_get_request(struct bitfield *me, struct bitfield *peer,
                         int *idx, int *offset, int *size);

#ifdef __cplusplus
extern "C" }
#endif

#endif
