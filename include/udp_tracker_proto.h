#ifndef UDP_TRACKER_PROTO_H
#define UDP_TRACKER_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    UDP_REQ_ACTION_CONNECT = 0,
    UDP_REQ_ACTION_ANNOUNCE_REQ = 1,
    UDP_REQ_ACTION_SCRAPED = 2,
    UDP_REQ_ACTION_ERROR = 3,
};

struct udp_req_header {
    unsigned long long conn_id;
    int action;
    int transaction_id;
};

struct udp_rsp_header {
    int action;
    int transaction_id;
};

#ifdef __cplusplus
extern "C" }
#endif

#endif
