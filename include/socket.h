#ifndef SOCKET_H_
#define SOCKET_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "btype.h"

struct addrinfo;

int get_ip_address_info(struct tracker_prot *tp, int *ip, uint16 *port);

int set_socket_unblock(int fd);
int set_socket_opt(int fd);
int get_socket_opt(int sfd, int opname, int *flags);
int get_socket_error(int sfd, int *error);

int socket_tcp_send(int sfd, char *buf, int buflen, int flags);
int socket_tcp_send_until_block(int sfd, char *buf, int buflen);
int socket_tcp_send_all(int sfd, char *buf, int buflen);

struct iovec;
int socket_tcp_send_iovs(int fd, const struct iovec *iov, int iovcnt);

int socket_tcp_recv(int sfd, char *buf, int buflen, int flags);
int socket_tcp_recv_until_block(int fd, char **dst, int *dstlen);

int socket_tcp_create(void);
int socket_tcp_connect(int sock, int ip, unsigned short port);
int socket_tcp_bind(int sock, int ip, uint16 port);
int socket_tcp_listen(int sock, int backlog);
int socket_tcp_accept(int sock, int *ip, uint16 *port);

int socket_udp_create(void);
int socket_udp_connect(int sock, int ip, unsigned short port);
int socket_udp_bind(int sock, int ip, uint16 port);
int socket_udp_send(int sfd, char *buf, int buflen, int flags);
int socket_udp_recv(int sfd, char *buf, int buflen, int flags);

struct sockaddr;
struct socklen_t;
int socket_udp_recvfrom(int sfd, char *buf, int buflen, int flags,
                                struct sockaddr *sa, socklen_t  *sl);

uint64 socket_hton64(uint64 host);
uint64 socket_ntoh64(uint64 net);

unsigned int socket_ntohl(unsigned int net);
unsigned short socket_ntohs(unsigned short net);

unsigned int socket_htonl(unsigned int host);
unsigned short socket_htons(unsigned short host);

#ifdef __cplusplus
extern "C" }
#endif

#endif
