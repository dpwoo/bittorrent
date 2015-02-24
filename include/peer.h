#ifndef PEER_H
#define PEER_H

#ifdef __cplusplus
extern "C" {
#endif

struct tracker;
int peer_init(struct tracker *tr, char *addrinfo);

#ifdef __cplusplus
extern "C" }
#endif

#endif
