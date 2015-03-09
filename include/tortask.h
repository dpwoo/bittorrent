#ifndef TORTASK_H
#define TORTASK_H

#ifdef __cplusplus
extern "C" {
#endif

struct tracker;
struct torrent_task;

int torrent_task_init(struct torrent_task *tsk, int epfd, char *torfile);

int torrent_timeout_handle(int event, void *evt_ctx);

int torrent_add_peer_addrinfo(struct torrent_task *tsk, char *peer);

int torrent_peer_recycle(struct torrent_task *tsk, struct peer *pr, int isactive);

int torrent_tracker_recycle(struct torrent_task *tsk, struct tracker *tr, int isactive);

int torrent_add_having_piece(struct torrent_task *tsk, int idx);

#ifdef __cplusplus
extern "C" }
#endif

#endif
