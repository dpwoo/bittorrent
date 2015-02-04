#include <stdio.h>
#include "btype.h"

extern int torrent_file_parser(char *torfile, struct torrent_file *tor);
extern int torrent_info_parser(struct torrent_file *tor);

static int usage(void)
{
    fprintf(stderr, "bittorrent torfile\n");
    return -1;
}

int main(int argc, char *argv[])
{
    if(argc != 2) {
        return usage();
    }

    struct torrent_file tor;
    if(torrent_file_parser(argv[1], &tor)) {
        fprintf(stderr, "parser %s failed!\n", argv[1]);
        return -1;
	} else {
    	fprintf(stderr, "parser %s ok!\n", argv[1]);
	}

	if(torrent_info_parser(&tor)) {
		fprintf(stderr, "parser torrent info failed!");
		return -1;
	} else {
		fprintf(stderr, "parser torrent info ok!\n");
	}

    return 0;
}
