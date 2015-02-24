#ifndef HTTP_H
#define HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "btype.h"

struct http_rsp_buf {
    char *rcvbuf;
    int rcvsz;
    char *body;
    int bodysz;
};

int http_request(struct tracker *tr);

int http_response(struct tracker *tr, struct http_rsp_buf *rspbuf);

#ifdef __cplusplus
extern "C" }
#endif

#endif
