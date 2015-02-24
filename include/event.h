#ifndef EVENT_H 
#define EVENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*event_handle_t)(int event, void *evt_ctx);

struct event_param {
    int fd, event;
    void *evt_ctx;
    event_handle_t evt_hdl;
};

int event_create(void);

int event_add(int epfd, struct event_param *ep);

int event_mod(int epfd, struct event_param *ep);

int event_del(int epfd, struct event_param *ep);

int event_loop(int epfd);

#ifdef __cplusplus
extern "C" }
#endif

#endif
