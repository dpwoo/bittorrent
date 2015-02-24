#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h> // inet_pton, inet_ntop
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "socket.h"
#include "log.h"
#include "utils.h"

int
free_ip_address_info(struct addrinfo *ai)
{
    if(ai) {
        freeaddrinfo(ai);
    }
    return 0;
}

int
get_ip_address_info(struct tracker_prot *tp, struct addrinfo **res)
{
    struct addrinfo hints;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if(tp->prot_type == TRACKER_PROT_UDP) {
        hints.ai_socktype = SOCK_DGRAM;
    }

    LOG_INFO("looking up %s:%s ...\n", tp->host, tp->port);

    int ret = getaddrinfo(tp->host, tp->port, &hints, res);
    if(ret) {
        LOG_INFO("getaddrinfo (%s:%s):%s\n", tp->host, tp->port, gai_strerror(ret));
        return -1;
    }

    return 0; 
}

int
create_tracker_client_socket(struct tracker_prot *tp, struct addrinfo *ai)
{
    int sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if(sfd < 0) {
        LOG_ERROR("socket error:%s\n", strerror(errno));
        return -1;
    }

    if(set_socket_unblock(sfd)) {
        LOG_ERROR("set socket unblock error:%s\n", strerror(errno));
        close(sfd);
        return -1;
    }

    if(tp->prot_type != TRACKER_PROT_UDP && set_socket_opt(sfd)) {
        LOG_ERROR("set socket opt error:%s\n", strerror(errno));
        close(sfd);
        return -1;
    }

    return sfd;
}

int
connect_tracker_server(int fd, struct tracker_prot *tp, struct addrinfo *ai, int *errNo)
{
    while(ai) {
        if(!connect(fd, ai->ai_addr, ai->ai_addrlen)) {
            LOG_INFO("connect (%s:%s)...ok\n", tp->host, tp->port);
            return 0;
        }

        if(tp->prot_type != TRACKER_PROT_UDP && errno == EINPROGRESS) {
            *errNo = EINPROGRESS;
            LOG_INFO("connect (%s:%s)...in progress\n", tp->host, tp->port);
            return 0;
        }

        LOG_INFO("connect (%s:%s)...failed:%s\n", tp->host, tp->port, strerror(errno));

        ai = ai->ai_next;
    }

    return -1;
}

int
set_socket_unblock(int sfd)
{    
    int flags;

    if((flags = fcntl(sfd, F_GETFL, 0)) < 0) {
        LOG_ERROR("fcntl %d get failed!\n", sfd);
        return -1;
    }

    flags |= O_NONBLOCK;

    if(fcntl(sfd, F_SETFL, flags) < 0) {
        LOG_ERROR("fcntl %d set failed!\n", sfd);
        return -1;
    }

    return 0;
}

int
set_socket_opt(int sfd)
{
    int flags = 1;
    return setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(flags));
}

int
get_socket_opt(int sfd, int opname, int *flags)
{
    socklen_t slen = sizeof(int);
    return getsockopt(sfd, SOL_SOCKET, opname, flags, &slen);
}

int
get_socket_error(int sfd, int *error)
{
    socklen_t slen = sizeof(int);
    return getsockopt(sfd, SOL_SOCKET, SO_ERROR, error, &slen);
}

int
socket_tcp_send(int sfd, char *buf, int buflen, int flags)
{
    return send(sfd, buf, buflen, flags);
}

int
socket_tcp_recv(int sfd, char *buf, int buflen, int flags)
{
    return recv(sfd, buf, buflen, flags);
}

int
socket_tcp_create(void)
{
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sfd < 0) {
        LOG_ERROR("socket:%s\n", strerror(errno));
        return -1;
    }

    return sfd;
}

int
socket_tcp_connect(int sock, int ip, unsigned short port)
{
    struct sockaddr_in sa;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = port;
    sa.sin_addr.s_addr = ip;

    return connect(sock, (struct sockaddr *)&sa, sizeof(sa));
}

unsigned int
socket_ntohl(unsigned int net)
{
    return ntohl(net);
}

unsigned int
socket_htonl(unsigned int host)
{
    return htonl(host);
}

unsigned short 
socket_ntohs(unsigned short net)
{
    return ntohs(net);
}

unsigned short 
socket_htons(unsigned short host)
{
    return htons(host);
}

#define ENLARGE_STEP (1024)

int
socket_tcp_recv_until_block(int fd, char **dst, int *dstlen)
{
    if(fd < 0 || !dst || !dstlen) {
        LOG_ERROR("invalid param!\n");
        return -1;
    }

    char *rspbuf = NULL;
    int mallocsz = 0, rcvtotalsz = 0;

    while(1) {

        if(mallocsz <= rcvtotalsz && utils_enlarge_space(&rspbuf, &mallocsz, ENLARGE_STEP)) {
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
            LOG_ERROR("socket recv failed:%s\n", strerror(errno));
            break;
        }
    }

    if(rcvtotalsz) {
        *dst = rspbuf;
        *dstlen = rcvtotalsz;
    } else {
        free(rspbuf);
    }

#if 0
    LOG_DEBUG("socket recv totalsz = %d\n", rcvtotalsz);
#endif

    return rcvtotalsz;
}

int
socket_tcp_send_until_block(int fd, char *sndbuf, int buflen)
{
    if(fd < 0 || !sndbuf || buflen <= 0) {
        return -1;
    }

    int totalsnd = 0;
    char *s = sndbuf;
    while(buflen > 0) {
        int wlen = socket_tcp_send(fd, s, buflen, 0);
        if(wlen > 0) {
            buflen -= wlen;
            s += wlen;
            totalsnd += wlen;
        } else if(wlen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            LOG_ERROR("socket send:%s\n", strerror(errno));
            return -1;
        }
    }

    return totalsnd;
}

int
socket_tcp_send_all(int fd, char *sndbuf, int buflen)
{
    if(fd < 0 || !sndbuf || buflen <= 0) {
        return -1;
    }

    char *s = sndbuf;
    while(buflen > 0) {
        int wlen = socket_tcp_send(fd, s, buflen, 0);
        if(wlen > 0) {
            buflen -= wlen;
            s += wlen;
        } else if(wlen < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            LOG_ERROR("socket send:%s\n", strerror(errno));
            return -1;
        }
    }

    return 0;
}

int
socket_tcp_send_iovs(int fd, const struct iovec *iov, int iovcnt)
{
    if(fd < 0 || !iov || iovcnt <= 0) {
        return -1;
    }
    return writev(fd, iov, iovcnt);
}

