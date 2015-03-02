#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "btype.h"
#include "event.h"
#include "tracker.h"
#include "torrent.h"
#include "log.h"
#include "bitfield.h"
#include "utils.h"

static int
usage(void)
{
    LOG_ERROR("Usage:./bittorrent torfile\n");
    return -1;
}

static int
test_one_task(char *torfile, struct torrent_task *tsk)
{
    if(torrent_file_parser(torfile, &tsk->tor)) {
        LOG_ERROR("parser %s failed!\n", torfile);
        return -1;
	}

	if(torrent_info_parser(&tsk->tor)) {
		LOG_ERROR("parser torrent info failed!\n");
		return -1;
	}

    if(bitfield_create(&tsk->bf, tsk->tor.pieces_num, tsk->tor.piece_len, tsk->tor.totalsz)) {
        LOG_ERROR("bitfield creat failed!\n");
        return -1;
    }

    if(torrent_create_downfiles(tsk)) {
        LOG_ERROR("torrent create downfile failed!\n");
        return -1;
    }

    torrent_check_downfiles_bitfield(tsk);

    if(tracker_announce(tsk)) {
        LOG_ERROR("tracker announce failed!\n");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if(argc != 2) {
        return usage();
    }

    if(utils_set_rlimit_core(0)) {
        LOG_ERROR("setrlimit failed:%s\n", strerror(errno));
        return -1;
    }

    set_log_level(LOG_LEVEL_DEBUG, LOG_TIME_FMT_SHORT);

    if(setenv("TZ", "GMT-8", 1)) {
        LOG_ERROR("set timezone failed:%s!\n", strerror(errno));
    }
    
    struct torrent_task tsk;
    memset(&tsk, 0, sizeof(tsk));

    tsk.epfd = event_create();
    if(tsk.epfd < 0) {
        LOG_ERROR("event_create failed!\n");           
        return -1;
    }
    
    if(test_one_task(argv[1], &tsk)) {
        LOG_ERROR("test_one_task failed!\n");           
        return -1;
    }

    LOG_INFO("main thread enter event loop...\n");

    if(event_loop(tsk.epfd)) {
        LOG_FATAL("event loop failed!\n");
        return -1;
    }

    return 0;
}

