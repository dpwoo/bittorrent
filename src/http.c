#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include "socket.h"
#include "http.h"
#include "log.h"
#include "mempool.h"

static int
is_rfc2396_alnum (char ch)
{
  return ('0' <= ch && ch <= '9')
      || ('A' <= ch && ch <= 'Z')
      || ('a' <= ch && ch <= 'z')
      || ch == '.'
      || ch == '-'
      || ch == '_'
      || ch == '~';
}

static char* 
http_escape_info_hash(const char *info_hash, char *esc_info_hash)
{
    char *dst = esc_info_hash;
    const char *src = info_hash;
    const char *src_end = info_hash + SHA1_LEN;

    while(src < src_end){
        if(is_rfc2396_alnum(*src)) {
            *dst++ = *src++;
        } else {
            dst += snprintf(dst, 4, "%%%02X", (unsigned char)*src++);
        }
    }
    *dst = '\0';
     
    return 0;
}

static int
http_build_uri(struct tracker *tr, char *uribuf, int buflen)
{
    #define URI_FMT_PARAM  \
            "%s"\
            "?info_hash=%s"\
            "&peer_id=%s"\
            "&port=%hu"\
            "&uploaded=%lld"\
            "&downloaded=%lld"\
            "&left=%lld"\
            "&compact=%d"

    char escape_info_hash[SHA1_LEN*3 + 1];

    http_escape_info_hash(tr->tsk->tor.info_hash, escape_info_hash);

    int wlen = snprintf(uribuf, buflen,
            URI_FMT_PARAM,
            tr->tp.reqpath, 
            escape_info_hash,
            peer_id,
            tr->tsk->listen_port,
            (int64)0,
            (int64)0,
            (int64)tr->tsk->leftpieces * tr->tsk->tor.piece_len,
            1
            );

    if(!tr->announce_cnt) {
        wlen += snprintf(uribuf+wlen, buflen-wlen, "&event=%s", "started"); 
    }

#if 0
    wlen += snprintf(uribuf+wlen, buflen-wlen, "&numwant=%d", 200);
#endif

#if 0
    LOG_DEBUG("tracker uri:%s\n", uribuf);
#endif

    return 0;
}

static int
http_write_request(int fd, char *reqbuf, int buflen)
{
    char *s = reqbuf;

    while(buflen > 0) {
        int wlen = socket_tcp_send(fd, s, buflen, 0);
        if(wlen > 0) {
            buflen -= wlen;
            s += wlen;
        } else if(wlen == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            LOG_INFO("write to tracker failed:%s\n", strerror(errno));
            return -1;
        }
    }
    
    return 0;
}

int
http_request(struct tracker *tr)
{
    char uribuf[1024], reqbuf[2048];

    http_build_uri(tr, uribuf, sizeof(uribuf));

    int reqlen = snprintf(reqbuf, sizeof(reqbuf), "GET %s HTTP/1.0\r\n\r\n", uribuf);

    if(http_write_request(tr->sockid, reqbuf, reqlen)) {
        return -1;
    }

    return 0;
}

static int
enlarge_response_space(char **dst, int *dstlen)
{
    char *tmp;
    if((tmp = GREALLOC(*dst, *dstlen + 1024))) {
        *dst = tmp;
        *dstlen += 1024;
    }
    return tmp ? 0 : -1;
}

static int
http_read_response(int fd, struct http_rsp_buf *hrb)
{
    char *rspbuf = NULL;
    int mallocsz = 0, rcvtotalsz = 0;

    while(1) {

        if(mallocsz <= rcvtotalsz && enlarge_response_space(&rspbuf, &mallocsz)) {
                break;
        }

        int rcvlen = socket_tcp_recv(fd, rspbuf+rcvtotalsz, mallocsz - rcvtotalsz, 0);

        if(rcvlen > 0) {
            rcvtotalsz += rcvlen;
        } else if(!rcvlen) {
            /* peer close */
            break;
        } else if(rcvlen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            LOG_INFO("recv tracker failed:%s\n", strerror(errno));
            break;
        }
    }

    hrb->rcvbuf = rspbuf;
    hrb->rcvsz = rcvtotalsz;
    hrb->body = NULL;
    hrb->bodysz = 0;

    return rcvtotalsz;
}

static int
http_status_line(char *rspbuf, int rsplen)
{
    char *buf, *s;
    
    if(!(buf = GMALLOC(rsplen+1))) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }
    memcpy(buf, rspbuf, rsplen);
    buf[rsplen] = '\0';

    if(!(s = strstr(buf, "HTTP/1.0")) &&  !(s = strstr(buf, "HTTP/1.1")) ) {
        GFREE(buf);
        return -1;
    }

    s += 8;

    errno = 0;
    int status = strtol(s, NULL, 10);
    if(errno || status != 200) {
        LOG_INFO("tracker http status %d\n", status);
        GFREE(buf);
        return -1;
    }

    GFREE(buf);

    return 0;
}

static int
http_get_content_length(char *rspbuf, int rsplen)
{
    char *buf, *s;
    
    if(!(buf = GMALLOC(rsplen + 1))) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }
    memcpy(buf, rspbuf, rsplen);
    buf[rsplen] = '\0';

#define CONTENT_LENGTH "Content-Length:"
    if(!(s = strstr(buf, CONTENT_LENGTH))) {
        GFREE(buf);
        return -1;
    }

    s += strlen(CONTENT_LENGTH);

    errno = 0;
    int con_len = strtol(s, NULL, 10);
    if(errno) {
        GFREE(buf);
        return -1;
    }

    GFREE(buf);

    return con_len;
}

static int
http_have_head(char *rspbuf, int rsplen)
{
    char *s = rspbuf;
    char *bufend = s + rsplen;

    while(s < bufend && (s = memchr(s, '\r', bufend - s))) {
        if((s+4) < bufend && s[0] == '\r' && s[1] == '\n' && s[2] == '\r' && s[3] == '\n') {
            return s+4 - rspbuf;
        }
        s++;
    }
    
    return -1;
}

static int
http_parser_head(char *rspbuf, int rsplen)
{
    int hdrlen;
    if((hdrlen = http_have_head(rspbuf, rsplen)) < 0) {
        return -1;
    }

    if(http_status_line(rspbuf, rsplen)) {
        return -1;
    }

    int con_len;
    con_len = http_get_content_length(rspbuf, rsplen);
    if(con_len > 0 && con_len != rsplen - hdrlen) {
        LOG_ALARM("warning, http content len[%d] != body len[%d]\n",
                 con_len, rsplen - hdrlen);
    }

    return hdrlen;
}

static int
http_parser_body(char *rspbuf, int rsplen)
{

#if 0
    LOG_DUMP(rspbuf, rsplen);
#endif

    return 0;
}

static int
http_response_parser(struct http_rsp_buf *hrb)
{
    if(hrb->rcvsz <= 0) {
        return -1;
    }

    int hdrlen;
    hdrlen = http_parser_head(hrb->rcvbuf, hrb->rcvsz);
    if(hdrlen <= 0 || hrb->rcvsz <= hdrlen) {
        return -1;
    }

    hrb->body = hrb->rcvbuf + hdrlen;
    hrb->bodysz = hrb->rcvsz - hdrlen;

    http_parser_body(hrb->body, hrb->bodysz);

    return 0;
}

int
http_response(struct tracker *tr, struct http_rsp_buf *rspbuf)
{
    if(http_read_response(tr->sockid, rspbuf) <= 0) {
        return -1;
    }

#if 0
    LOG_DUMP(rspbuf->rcvbuf, rspbuf->rcvsz);
#endif

    if(http_response_parser(rspbuf)) {
        return -1;
    }

    return 0;
}

