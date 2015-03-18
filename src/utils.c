#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include "utils.h"
#include "log.h"
#include "socket.h"
#include "sha1.h"
#include "mempool.h"

int
utils_enlarge_space(char **dst, int *dstlen, int step)
{
    if(*dstlen + step <= 0) {
        return -1;
    }

    char *tmp;
    if((tmp = GREALLOC(*dst, *dstlen + step))) {
        *dst = tmp;
        *dstlen += step;
    }

    return tmp ? 0 : -1;
}

char* 
utils_strf_addrinfo(int ip,unsigned short port, char *addrbuf, int buflen)
{
    char *s = (char *)&ip;

    snprintf(addrbuf, buflen, "%03d.%03d.%03d.%03d:%05hu",
                        (unsigned char)s[0],
                        (unsigned char)s[1],
                        (unsigned char)s[2],
                        (unsigned char)s[3],
                        socket_ntohs(port));
    return addrbuf;
}

int
utils_set_rlimit_core(int msize)
{
    struct rlimit limit;
    limit.rlim_cur = msize <= 0 ? RLIM_INFINITY : 1024*1024*msize; 
    limit.rlim_max = msize <= 0 ? RLIM_INFINITY : 1024*1024*msize; 
    return setrlimit(RLIMIT_CORE, &limit);
}

/* should add -D_FILE_OFFSET_BITS=64 makefile */
int64
utils_lseek(int fd, int64 offset, int whence)
{	
	if(fd < 0 || offset < 0 || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)) {
		LOG_ERROR("invalid param!\n");
		return -1;
	}

	return lseek(fd, offset, whence);
}

int
utils_sha1_check(const char *buffer, int buflen, const char *sha1, int sha1len)
{
    if(!buffer || buflen <= 0 || !sha1 || sha1len != 20) {
        return -1;
    }

    char gensha1[20];
    if(utils_sha1_gen(buffer, buflen, gensha1, 20)) {
       LOG_ERROR("sha1 generate failed!\n");
       return -1;
    }

    return !memcmp(sha1, gensha1, 20) ? 0 : -1;
}

int
utils_sha1_gen(const char *buffer, int buflen, char *sha1, int sha1len)
{
    if(!buffer || buflen <= 0 || !sha1 || sha1len != 20) {
        return -1;
    }

    struct SHA1Context ctx;

    SHA1Reset(&ctx);

    SHA1Input(&ctx, (unsigned char *)buffer, buflen);

    if(!SHA1Result(&ctx)) {
        LOG_ERROR("compute memssage Digest failed!\n");
        return -1;
    }

    int i;
    for(i = 0; i < 5; i ++) {
        int bigend = ctx.Message_Digest[i];
        bigend = socket_htonl(bigend);
        memcpy(sha1+i*4, &bigend, 4);
    }

#if 0
    char info_hash_hex[40+1];

    snprintf(info_hash_hex, sizeof(info_hash_hex), "%08X%08X%08X%08X%08X",
            ctx.Message_Digest[0],
            ctx.Message_Digest[1],
            ctx.Message_Digest[2],
            ctx.Message_Digest[3],
            ctx.Message_Digest[4]
            );
    LOG_DEBUG("info_hash_hex:%.40s\n", info_hash_hex);

#endif

    return 0;
}

int
utils_url_parser(const char *url, struct tracker_prot *tp)
{
	memset(tp, 0, sizeof(*tp));

	char *str = GSTRDUP(url);
	if(!str) {
		LOG_ERROR("out of memory!\n");
		return -1;
	}

	char *s;
    if(strstr(str, "http://")) {
        s  = str + 7;
        tp->prot_type = TRACKER_PROT_HTTP;
    } else if(strstr(str, "udp://")) {
        s = str + 6; 
        tp->prot_type = TRACKER_PROT_UDP;
    } else {
		LOG_INFO("not support tracker proto[%s]!\n", url);
		GFREE(str);
		return -1;
    }

	char *path = strchr(s, '/');
	if(path) {
		*path++ = '\0';
	}

	char *port = strchr(s, ':');
	if(port) {
		*port++ = '\0';
		if(!isdigit(*port)) {
			LOG_INFO("invalid url[%s]!\n", url);
			GFREE(str);
			return -1;
		}
        tp->port = GSTRDUP(port);
	} else {
        tp->port = GSTRDUP("80");
    }

	tp->host = GSTRDUP(s);

	if(!path || *path == '\0') {
		tp->reqpath = GSTRDUP("/");
	} else {
		int slen = strlen(path);
		char buf[slen+2];
		snprintf(buf, sizeof(buf), "/%s", path);
		tp->reqpath = GSTRDUP(buf);
	}

	if(!tp->host || !tp->reqpath || !tp->port) {
		LOG_ERROR("out of memory!\n");
		GFREE(tp->host);
		GFREE(tp->reqpath);
        GFREE(tp->port);
		GFREE(str);
		return -1;
	}

	GFREE(str);

	return 0;
}

