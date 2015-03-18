#ifndef TRACKER_H_
#define TRACKER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "event.h"

struct tracker;

int tracker_http_announce(struct tracker *tr);
int tracker_udp_announce(struct tracker *tr);

int tracker_reset_members(struct tracker *tr);

int tracker_add_event(int event, struct tracker *tr, event_handle_t handle);
int tracker_mod_event(int event, struct tracker *tr, event_handle_t handle);
int tracker_del_event(struct tracker *tr);

int tracker_create_timer(struct tracker *tr, event_handle_t handle);
int tracker_start_timer(struct tracker *tr, int time);
int tracker_stop_timer(struct tracker *tr);
int tracker_destroy_timer(struct tracker *tr);

#ifdef __cplusplus
extern "C" }
#endif

#endif
