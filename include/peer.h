#ifndef PEER_H
#define PEER_H

#ifdef __cplusplus
extern "C" {
#endif

int peer_init(struct peer *tr);

int peer_modify_timer_time(struct peer *pr, int time);

#ifdef __cplusplus
extern "C" }
#endif

#endif
