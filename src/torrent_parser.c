#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "btype.h"
#include "torrent.h"
#include "log.h"

struct benc_type* 
get_dict_value_by_key(struct benc_type *bt, const char *str, int val_type)
{
    int i, slen = strlen(str);

	for(i = 0; i < bt->val.list.nlist; i += 2) {
		struct benc_type *key = bt->val.list.vals+i;
		struct benc_type *val = bt->val.list.vals+i+1;

		if(key->type != BENC_TYPE_STRING || val->type != val_type) {
			continue;
		}

		if(!memcmp(str, key->val.str.s, slen)) {
			return val;
		}
	}

	return NULL;
}

int
handle_string_kv(struct benc_type *bt, const char *str, char **setme, int *setmelen)
{
	struct benc_type *val;

	val = get_dict_value_by_key(bt, str, BENC_TYPE_STRING);
	if(!val) {
		return -1;
	}

	int slen = val->val.str.len;
	if((*setme = malloc(slen+1)) == NULL) {
		LOG_ERROR("out of memory!\n");
		return -1;
	}
	(*setme)[slen] = '\0';
	memcpy(*setme, val->val.str.s, slen);

    if(setmelen) {
        *setmelen = slen;
    }

	return 0;
}

int
handle_int64_kv(struct benc_type *bt, const char *str, int64 *setme)
{
	struct benc_type *val;

	val = get_dict_value_by_key(bt, str, BENC_TYPE_INT);
	if(!val) {
		return -1;
	}

	*setme = val->val.i;

	return 0;
}

int
handle_int_kv(struct benc_type *bt, const char *str, int32 *setme)
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
    int ret = handle_string_kv(&tor->bt, "announce", &tor->tracker_url[0], NULL);
    if(!ret) {
        tor->tracker_num = 1;
    }
    return ret;
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
	
	int i, k = tor->tracker_num;

	for(i = 0; i < list->val.list.nlist; i++) {
		struct benc_type *url_list, *url;

		url_list = list->val.list.vals + i;
        if(url_list->type != BENC_TYPE_LIST) {
            LOG_ALARM("warn! Not List type, skiped!\n");
            continue;
        }

		url = url_list->val.list.vals;
        if(url->type != BENC_TYPE_STRING) {
            LOG_ALARM("warn! Not string type, skiped!\n");
            continue;
        }

		int slen = url->val.str.len;
        if(i == 0 && tor->tracker_url[0] && !memcmp(tor->tracker_url[0], url->val.str.s, slen)) {
            continue;
        }

		if((tor->tracker_url[k] = malloc(slen + 1)) == NULL) {
			LOG_ERROR("out of memory!\n");
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
        LOG_ERROR("invalid pieces sha1 len[%d]!\n", slen);
        return -1;
    }
    tor->pieces_num = slen / SHA1_LEN;

    return 0;
}

static int
handle_info_length_kv(struct benc_type *bt, struct torrent_file *tor)
{
    if(handle_int64_kv(bt, "length", &tor->totalsz)) {
        return -1;
    }

    if(get_dict_value_by_key(bt, "files", BENC_TYPE_DICT)) {
        LOG_ERROR("both length and files key exist!\n");
        return -1;
    }

    tor->isSingleDown = 1;

    return 0;
}

static int
handle_subdir_path(struct benc_type *file, char **subdir)
{
    int i, totalsz = 0;
    for(i = 0; i < file->val.list.nlist-1; i++) {
        struct benc_type *path;
        
        path = file->val.list.vals + i;
        if(!path || path->type != BENC_TYPE_STRING) {
            LOG_ERROR("%s:error path string!\n", __func__);
            return -1;
        }

        totalsz += path->val.str.len + 1;//add 1 for '/'
    }

    if(!totalsz) {
        return -1;
    }

#if 0
    char *s, *pathname = malloc(totalsz+1);
#else
    char *s, *pathname = malloc(totalsz);
#endif

    if(!pathname) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

//  LOG_DEBUG("\n\n\n");
    s = pathname;
    for(i = 0; i < file->val.list.nlist-1; i++) {
        int slen;
        struct benc_type *path;

        path = file->val.list.vals + i;
        slen = path->val.str.len;
        memcpy(s, path->val.str.s, slen);

//      LOG_DUMP(path->val.str.s, path->val.str.len, "str:");
//      LOG_DEBUG("str:%s\n", path->val.str.s);

#if 0
        s[slen] = '/';
        s += slen+1;
#else
        if(i+1 != file->val.list.nlist-1) {
            s[slen] = '/';
            s += slen+1;
        } else { /* the last dir not need '/' and for the '\0' */
            s += slen;
        }
#endif

    }
    pathname[totalsz-1] = '\0';
    *subdir = pathname;

    return 0;
}

static int
handle_info_file_kv(struct benc_type *file, struct single_file *sf)
{
    if(handle_int64_kv(file, "length", &sf->file_size)) {
        LOG_ERROR("mfile length key failed!\n");
        return -1;
    }

    struct benc_type *pathlist;
    pathlist = get_dict_value_by_key(file, "path", BENC_TYPE_LIST);
    if(!pathlist) {
        LOG_ERROR("no found path key!\n");
        return -1;
    }

    int nlist = pathlist->val.list.nlist;

    if(nlist > 1) {
        if(handle_subdir_path(pathlist, &sf->subdir)) {
            return -1;
        }
    }

    struct benc_type *path;

    path = pathlist->val.list.vals + nlist - 1;
    if(!path || path->type != BENC_TYPE_STRING) {
        LOG_ERROR("error path string!\n");
        return -1;
    }

    int slen = path->val.str.len;
    if(!(sf->pathname = malloc(slen+1))) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }
    memcpy(sf->pathname, path->val.str.s, slen+1);

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
        LOG_ERROR("both length and files key exist!\n");
        return -1;
    }

    if(!(tor->mfile.files_num = files->val.list.nlist)) {
        LOG_ERROR("error, files num is 0!\n");
        return -1;
    }

    tor->mfile.files = calloc(1, sizeof(struct single_file) * tor->mfile.files_num);
    if(!tor->mfile.files_num || !tor->mfile.files) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    tor->totalsz = 0;
    tor->isSingleDown = 0;

    int i;
    for(i = 0; i < files->val.list.nlist; i++) {
        struct benc_type *file;

        file = files->val.list.vals + i;
        if(file->type != BENC_TYPE_DICT) {
            LOG_ERROR("file type should be %d, but %d\n", BENC_TYPE_DICT, file->type);
            return -1;
        }
        
        if(handle_info_file_kv(file, tor->mfile.files + i)) {
            LOG_ERROR("handle_info_file faled!\n");
            return -1;
        }

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
        LOG_ERROR("no found name key!\n");
        return -1;
    }

    if(handle_info_piece_length_kv(info, tor)) {
        LOG_ERROR("no found piece length key!\n");
        return -1;
    }

    if(handle_info_pieces_kv(info, tor)) {
        LOG_ERROR("no found pieces key!\n");
        return -1;
    }

    if(handle_info_length_kv(info, tor) && handle_info_files_kv(info, tor)) {
        LOG_ERROR("handle length or files key failed!\n");
        return -1;
    }

	return 0;
}

static void
dump_torrent_info(struct torrent_file *tor)
{
    int i;

    if(tor->torfile) {
        LOG_DEBUG("dump: %s\n", tor->torfile);
    }

    if(tor->pathname) {
        LOG_DEBUG("pathname:%s\n", tor->pathname);
    }

    LOG_DEBUG("piece length:%d\n", tor->piece_len);
    LOG_DEBUG("total size:%lld\n", tor->totalsz);
    LOG_DEBUG("pieces num:%d\n", tor->pieces_num);

    if(tor->tracker_num) {
        for(i = 0; i < tor->tracker_num; i++) {
            LOG_DEBUG("url:%s\n", tor->tracker_url[i]);
        }
    }

    if(!tor->isSingleDown && tor->mfile.files_num) {
        for(i = 0; i < tor->mfile.files_num; i++) {
            LOG_DEBUG("file[%lld]:%s/%s\n",
                 tor->mfile.files[i].file_size, 
                 tor->mfile.files[i].subdir == NULL ? "" : tor->mfile.files[i].subdir,
                 tor->mfile.files[i].pathname);
        }
    }

    if(tor->comment) {
        LOG_DEBUG("comment:%s\n", tor->comment);
    }

    if(tor->creator) {
        LOG_DEBUG("creator:%s\n", tor->creator);
    }

    if(tor->create_date) {
        time_t date = (time_t)tor->create_date;
        struct tm *t = gmtime(&date);
        if(!t) {
            LOG_ERROR("gmtime failed!\n");
        }
        char timebuf[256];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d#%H:%M:%S", t);
        LOG_DEBUG("create date:%s\n", timebuf);
    }

}

int
torrent_info_parser(struct torrent_file *tor)
{
	handle_announce_kv(tor);
	handle_announcelist_kv(tor);

    if(!tor->tracker_num) {
        LOG_ERROR("torrent have no announce list!\n");
        return -1;
    }

	handle_comment_kv(tor);
	handle_create_date_kv(tor);
	handle_creator_kv(tor);

	if(handle_info_kv(tor)) {
		LOG_ERROR("handle info key failed!\n");
		return -1;
	}

#if 0
    dump_torrent_info(tor);
#endif

	return 0;
}

