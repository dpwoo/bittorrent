#ifndef SOCKET_H_
#define SOCKET_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "btype.h"

struct addrinfo;

int free_ip_address_info(struct addrinfo *ai);
int get_ip_address_info(struct tracker_prot *tp, struct addrinfo **res);

int http_url_parser(const char *url, struct tracker_prot *tp);

int create_tracker_client_socket(struct tracker_prot *tp, struct addrinfo *ai);
int connect_tracker_server(int fd, struct tracker_prot *tp,
                            struct addrinfo *ai, int *errNo);

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

unsigned int socket_ntohl(unsigned int net);
unsigned short socket_ntohs(unsigned short net);

unsigned int socket_htonl(unsigned int host);
unsigned short socket_htons(unsigned short host);

#ifdef __cplusplus
extern "C" }
#endif

#endif
