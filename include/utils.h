#ifndef UTILS_H
#define UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "type.h"

int utils_enlarge_space(char **dst, int *dstlen, int step);

char *utils_strf_addrinfo(int ip,unsigned short port, char *addrbuf, int buflen);

int utils_set_rlimit_core(int msize);

int64 utils_lseek(int fd, int64 offset, int whence);

int utils_sha1_check(const char *buffer, int buflen, const char *sha1, int sha1len);

int utils_sha1_gen(const char *buffer, int buflen, char *sha1, int sha1len);

#ifdef __cplusplus
extern "C" }
#endif

#endif
