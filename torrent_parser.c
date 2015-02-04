#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "btype.h"

static struct benc_type* 
get_dict_value_by_key(struct benc_type *bt, const char *str, int val_type)
{
	int i;
	for(i = 0; i < bt->val.list.nlist; i += 2) {
		struct benc_type *key = bt->val.list.vals+i;
		struct benc_type *val = bt->val.list.vals+i+1;

		if(key->type != BENC_TYPE_STRING || val->type != val_type) {
			continue;
		}

		if(!memcmp(str, key->val.str.s, strlen(str))) {
			return val;
		}
	}

	return NULL;
}

static int
handle_string_kv(struct benc_type *bt, const char *str, char **setme, int *setmelen)
{
	struct benc_type *val;

	val = get_dict_value_by_key(bt, str, BENC_TYPE_STRING);
	if(!val) {
		return -1;
	}

	int slen = val->val.str.len;
	if((*setme = malloc(slen+1)) == NULL) {
		fprintf(stderr, "out of memory!\n");
		return -1;
	}
	(*setme)[slen] = '\0';
	memcpy(*setme, val->val.str.s, slen);

    if(setmelen) {
        *setmelen = slen;
    }

	return 0;
}

static int
handle_int_kv(struct benc_type *bt, const char *str, int *setme)
{
	struct benc_type *val;

	val = get_dict_value_by_key(bt, str, BENC_TYPE_INT);
	if(!val) {
		return -1;
	}

	*setme = val->val.i;

	return 0;
}

static int
handle_announce_kv(struct torrent_file *tor)
{
	return handle_string_kv(&tor->bt, "announce", &tor->announce, NULL);
}

static int
handle_comment_kv(struct torrent_file *tor)
{
	return handle_string_kv(&tor->bt, "comment", &tor->comment, NULL);
}

static int
handle_creator_kv(struct torrent_file *tor)
{
	return handle_string_kv(&tor->bt, "created by", &tor->creator, NULL);
}

static int
handle_create_date_kv(struct torrent_file *tor)
{
	return handle_int_kv(&tor->bt, "creation date", &tor->create_date);
}

static int
handle_announcelist_kv(struct torrent_file *tor)
{
	struct benc_type *list;

	list = get_dict_value_by_key(&tor->bt, "announce-list", BENC_TYPE_LIST);
	if(!list) {
		return -1;
	}
	
	int i, k;
	for(i = k = 0; i < list->val.list.nlist; i++) {
		struct benc_type *url_list, *url;

		url_list = list->val.list.vals + i;
        if(url_list->type != BENC_TYPE_LIST) {
            fprintf(stderr, "warn! Not List type, skiped!\n");
            continue;
        }

		url = url_list->val.list.vals;
        if(url->type != BENC_TYPE_STRING) {
            fprintf(stderr, "warn! Not string type, skiped!\n");
            continue;
        }
		int slen = url->val.str.len;

		if((tor->tracker_url[k] = malloc(slen + 1)) == NULL) {
			fprintf(stderr, "out of memory!\n");
			continue;
		}
		tor->tracker_url[k][slen] = '\0';
		memcpy(tor->tracker_url[k], url->val.str.s, slen);

		if(++k >= MAX_TRACKER_NUM) {
			break;
		}
	}
	tor->tracker_num = k;

    return 0;
}

static int
handle_info_name_kv(struct benc_type *bt, struct torrent_file *tor)
{
    return handle_string_kv(bt, "name", &tor->pathname, NULL);
}

static int
handle_info_piece_length_kv(struct benc_type *bt, struct torrent_file *tor)
{
    return handle_int_kv(bt, "piece length", &tor->piece_len);
}

static int
handle_info_pieces_kv(struct benc_type *bt, struct torrent_file *tor)
{
    int slen;

    if(handle_string_kv(bt, "pieces", &tor->pieces, &slen)) {
        return -1;
    }

    if(slen <= 0 || slen % SHA1_LEN) {
        fprintf(stderr, "invalid pieces sha1 len[%d]!\n", slen);
        return -1;
    }
    tor->pieces_num = slen / SHA1_LEN;

    return 0;
}

static int
handle_info_length_kv(struct benc_type *bt, struct torrent_file *tor)
{
    if(handle_int_kv(bt, "length", &tor->totalsz)) {
        return -1;
    }

    if(get_dict_value_by_key(bt, "files", BENC_TYPE_DICT)) {
        fprintf(stderr, "both length and files key exist!\n");
        return -1;
    }

    tor->isSingleDown = 1;

    return 0;
}

static int
handle_info_files_kv(struct benc_type *bt, struct torrent_file *tor)
{
    struct benc_type *files;

    files = get_dict_value_by_key(bt, "files", BENC_TYPE_LIST);
    if(!files) {
        return -1;
    }

    if(get_dict_value_by_key(bt, "length", BENC_TYPE_INT)) {
        fprintf(stderr, "both length and files key exist!\n");
        return -1;
    }

    if(!(tor->mfile.files_num = files->val.list.nlist)) {
        fprintf(stderr, "error, files num is 0!\n");
        return -1;
    }

    tor->mfile.files = calloc(1, sizeof(struct single_file) * tor->mfile.files_num);
    if(!tor->mfile.files_num || !tor->mfile.files) {
        fprintf(stderr, "out of memory!\n");
        return -1;
    }

    tor->totalsz = 0;
    tor->isSingleDown = 0;

    int i;
    for(i = 0; i < files->val.list.nlist; i++) {
        struct benc_type *file;
        file = files->val.list.vals + i;
        if(file->type != BENC_TYPE_DICT) {
            fprintf(stderr, "file type should be %d, but %d\n", BENC_TYPE_DICT, file->type);
            return -1;
        }
        
        if(handle_int_kv(file, "length", &tor->mfile.files[i].file_size)) {
            fprintf(stderr, "mfile length key failed!\n");
            return -1;
        }

        struct benc_type *pathlist;
        pathlist = get_dict_value_by_key(file, "path", BENC_TYPE_LIST);
        if(!pathlist) {
            fprintf(stderr, "no found path key!\n");
            return -1;
        }
        
        struct benc_type *path = pathlist->val.list.vals;
        if(!path || path->type != BENC_TYPE_STRING) {
            fprintf(stderr, "error path string!\n");
            return -1;
        }

        int slen = path->val.str.len;
        if(!(tor->mfile.files[i].pathname = malloc(slen+1))) {
            fprintf(stderr, "out of memory!\n");
            return -1;
        }
        memcpy(tor->mfile.files[i].pathname, path->val.str.s, slen+1);

        tor->totalsz += tor->mfile.files[i].file_size;
    }

    return 0;
}

static int
handle_info_kv(struct torrent_file *tor)
{
	struct benc_type *info;

	info = get_dict_value_by_key(&tor->bt, "info", BENC_TYPE_DICT);
	if(!info) {
		return -1;
	}
    
    if(handle_info_name_kv(info, tor)) {
        fprintf(stderr, "no found name key!\n");
        return -1;
    }

    if(handle_info_piece_length_kv(info, tor)) {
        fprintf(stderr, "no found piece length key!\n");
        return -1;
    }

    if(handle_info_pieces_kv(info, tor)) {
        fprintf(stderr, "no found pieces key!\n");
        return -1;
    }

    if(handle_info_length_kv(info, tor) && handle_info_files_kv(info, tor)) {
        fprintf(stderr, "handle length or files key failed!\n");
        return -1;
    }

	return 0;
}

static void
dump_torrent_info(struct torrent_file *tor)
{
    int i;

    if(tor->torfile) {
        fprintf(stderr, "\ndump: %s\n", tor->torfile);
    }

    if(tor->pathname) {
        fprintf(stderr, "pathname:%s\n", tor->pathname);
    }

    fprintf(stderr, "piece length:%d\n", tor->piece_len);
    fprintf(stderr, "total size:%d\n", tor->totalsz);
    fprintf(stderr, "pieces num:%d\n", tor->pieces_num);

    if(tor->announce) {
        fprintf(stderr, "announce:%s\n", tor->announce);
    }

    if(tor->tracker_num) {
        for(i = 0; i < tor->tracker_num; i++) {
            fprintf(stderr, "url:%s\n", tor->tracker_url[i]);
        }
    }

    if(!tor->isSingleDown && tor->mfile.files_num) {
        for(i = 0; i < tor->mfile.files_num; i++) {
            fprintf(stderr, "file[%d]:%s\n",
                 tor->mfile.files[i].file_size, tor->mfile.files[i].pathname);
        }
    }

    if(tor->comment) {
        fprintf(stderr, "comment:%s\n", tor->comment);
    }

    if(tor->creator) {
        fprintf(stderr, "creator:%s\n", tor->creator);
    }

    if(tor->create_date) {
        time_t date = (time_t)tor->create_date;
        struct tm *t = gmtime(&date);
        if(!t) {
            fprintf(stderr, "gmtime failed!\n");
        }
        char timebuf[256];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d#%H:%M:%S", t);
        fprintf(stderr, "create date:%s\n", timebuf);
    }

}

int
torrent_info_parser(struct torrent_file *tor)
{
	if(handle_announce_kv(tor)) {
		fprintf(stderr, "no found announce key!\n");
		return -1;
	}

	if(handle_announcelist_kv(tor)) {
		fprintf(stderr, "no found announce list key!\n");
	}

	handle_comment_kv(tor);
	handle_create_date_kv(tor);
	handle_creator_kv(tor);

	if(handle_info_kv(tor)) {
		fprintf(stderr, "handle info key failed!\n");
		return -1;
	}

    dump_torrent_info(tor);

	return 0;
}

