#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include "btype.h"
#include "event.h"
#include "tracker.h"
#include "torrent.h"
#include "log.h"
#include "bitfield.h"
#include "utils.h"
#include "tortask.h"

static int
usage(void)
{
    LOG_ERROR("Usage:./bittorrent torfile\n");
    return -1;
}

int main(int argc, char *argv[])
{
    if(argc != 2) {
        return usage();
    }

    signal(SIGPIPE, SIG_IGN);

    set_log_level(LOG_LEVEL_DEBUG, LOG_TIME_FMT_SHORT);

    if(utils_set_rlimit_core(0)) {
        LOG_ERROR("setrlimit failed:%s\n", strerror(errno));
        return -1;
    }

    if(setenv("TZ", "GMT-8", 1)) {
        LOG_ERROR("set timezone failed:%s!\n", strerror(errno));
    }
    
    int epfd = event_create();
    if(epfd < 0) {
        LOG_ERROR("event_create failed!\n");           
        return -1;
    }

	struct torrent_task tsk;
    if(torrent_task_init(&tsk, epfd, argv[1])) {
        LOG_ERROR("torrent task init failed!\n");
        return -1;
    }

    LOG_INFO("main thread enter event loop...\n");

    if(event_loop(tsk.epfd)) {
        LOG_INFO("event loop quit!\n");
        return -1;
    }

    return 0;
}

