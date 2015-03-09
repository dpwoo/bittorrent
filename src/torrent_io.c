#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "btype.h"
#include "utils.h"
#include "log.h"

#define DOWNLOAD_DIR "./download/"

static int
torrent_create_dir(const char *dirname)
{
    char command[2048];
    /* 'dirname' in case space etc character when create dir */
    snprintf(command, sizeof(command), "%s %s%s%s", "mkdir -p", "\'", dirname, "\'"); 
    int ret = system(command);
    if(ret == -1) {
        return -1;
    }
    return 0;
}

static int
torrent_create_file(const char *subdir1, const char *subdir2, const char *filename, FILE **setme_fp)
{
    char dir[1024] = {'\0', };

    int offs = snprintf(dir, sizeof(dir), "%s", DOWNLOAD_DIR);

    if(subdir1) {
        offs += snprintf(dir+offs, sizeof(dir)-offs, "%s/", subdir1);
    }

    if(subdir2) {
        offs += snprintf(dir+offs, sizeof(dir)-offs, "%s", subdir2);
    }

    char fullname[2048];
    snprintf(fullname, sizeof(fullname), "%s/%s", dir, filename);

    struct stat st;
    if(stat(fullname, &st) || !S_ISREG(st.st_mode)) {
        if(torrent_create_dir(dir)) {
            LOG_ERROR("torrent create dir failed!\n");
            return -1;
        }
    }

    FILE *fp = fopen(fullname, "r+");
    if(!fp && !(fp = fopen(fullname, "w+")) ) {
        LOG_ERROR("fopen %s : %s\n", fullname, strerror(errno));
        return -1;
    }

    if(setme_fp) {
        *setme_fp = fp;
    } else {
        fclose(fp);
    } 

    return 0;
}

int
torrent_create_downfiles(struct torrent_task *tsk)
{
    if(tsk->tor.isSingleDown) {
        if(torrent_create_file(NULL, NULL, tsk->tor.pathname, NULL)) {
            LOG_ERROR("torrent create file failed!\n");
            return -1;
        }
        return 0;
    }

    int i;
    for(i = 0; i < tsk->tor.mfile.files_num; i++) {
        int ret = torrent_create_file(tsk->tor.pathname, tsk->tor.mfile.files[i].subdir,
                                                        tsk->tor.mfile.files[i].pathname, NULL);
        if(ret) {
            LOG_ERROR("torrent create file failed!\n");
            return -1;
        }
    }

    return 0;
}

static int
torrent_read_single_file(const char *subdir1, const char *subdir2, const char *filename, 
                                            int64 offset, char *buffer, int64 buflen)
{
    FILE *fp;
    if(torrent_create_file(subdir1, subdir2, filename, &fp)) {
        LOG_ERROR("create file for write failed!\n");
        return -1;
    }

    struct stat st;
    if(fstat(fileno(fp), &st)) {
        LOG_ERROR("fstat %s failed:%s\n", strerror(errno));
        fclose(fp);
        return -1;
    }

    if(st.st_size < offset + buflen) {
        /* LOG_ERROR("read piece failed, not enough data for reading!\n"); */
        fclose(fp);
        return -1;
    }

    if(utils_lseek(fileno(fp), offset, SEEK_SET) != offset) {
        LOG_ERROR("lseek %s:%s\n", filename, strerror(errno));
        fclose(fp);
        return -1;
    }

    if(fread(buffer, 1, buflen, fp) != buflen) {
        LOG_ERROR("fread %s:%s\n", buflen, filename, strerror(errno));
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

int
torrent_read_piece(struct torrent_task *tsk, int pieceid, char **setme_buffer, int *setme_buflen)
{
    if(!tsk || pieceid < 0 || pieceid >= tsk->tor.pieces_num || !setme_buffer || !setme_buflen) {
        LOG_ERROR("invalid param!\n");
        return -1;
    }

    int buflen = tsk->tor.piece_len;
    if(pieceid == tsk->tor.pieces_num - 1) {
        int last_piecesz = tsk->tor.totalsz  % tsk->tor.piece_len;
        buflen = last_piecesz ? last_piecesz : tsk->tor.piece_len;
    }

    char *buffer = malloc(buflen);
    if(!buffer) {
        LOG_ERROR("out of memory!\n");
        return -1;
    }

    int64 offset = (int64)tsk->tor.piece_len * pieceid;
    if(tsk->tor.isSingleDown) {
        int ret = torrent_read_single_file(NULL, NULL, tsk->tor.pathname,
                                         offset, buffer, buflen);
        if(ret) {
            free(buffer);
            return -1;
        }
        *setme_buffer = buffer;
        *setme_buflen = buflen;
        return 0;
    }

    int i;
    int64 invalid_offset = 0, fsize = 0;
    struct single_file *sfile =  tsk->tor.mfile.files;
    for(i = 0; i < tsk->tor.mfile.files_num; i++) {
        invalid_offset = fsize;
        fsize += sfile[i].file_size;
        if(fsize > offset) {
            break;
        }
    }
   
    if(i == tsk->tor.mfile.files_num) {
        LOG_ERROR("Error, can't find the file to read piece!\n");
        return -1;
    }

    int64 readsz = buflen; 
    int64 leftsz = offset + buflen - fsize;
    if(leftsz > 0) {
        readsz = buflen - leftsz;
    }

    if(torrent_read_single_file(tsk->tor.pathname, sfile[i].subdir, sfile[i].pathname,
                                            offset-invalid_offset, buffer, readsz)) {
        free(buffer);
        return -1; 
    }

    int64 bufoffset = readsz;
    for(i++; leftsz > 0 && i < tsk->tor.mfile.files_num; i++) {
        readsz = leftsz;
        leftsz = leftsz - sfile[i].file_size; 
        if(leftsz > 0) {
            readsz = sfile[i].file_size;
        }

        if(torrent_read_single_file(tsk->tor.pathname, sfile[i].subdir, sfile[i].pathname,
                                            0, buffer+bufoffset, readsz)) {
            free(buffer);
            return -1;
        }

        bufoffset += readsz;
    }

    *setme_buffer = buffer;
    *setme_buflen = buflen;
    return 0;
}

static int
torrent_write_single_file(const char *subdir1, const char *subdir2, const char *filename, 
                                                int64 offset, const char *buffer, int64 buflen)
{
    FILE *fp;
    if(torrent_create_file(subdir1, subdir2, filename, &fp)) {
        LOG_ERROR("create file for write failed!\n");
        return -1;
    }

    if(utils_lseek(fileno(fp), offset, SEEK_SET) != offset) {
        LOG_ERROR("lseek[%lld] %s:%s\n", offset, filename, strerror(errno));
        fclose(fp);
        return -1;
    }

    if(fwrite(buffer, 1, buflen, fp) != buflen) {
        LOG_ERROR("fwrite %s:%s\n", filename, strerror(errno));
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

int
torrent_write_piece(struct torrent_task *tsk, int pieceid, const char *buffer, int buflen)
{
    if(!tsk || !buffer || buflen <= 0 || pieceid < 0 || pieceid >= tsk->tor.pieces_num) {
        LOG_ERROR("invalid param!\n");
        return -1;
    }

    int64 offset = (int64)tsk->tor.piece_len * pieceid;
    if(tsk->tor.isSingleDown) {
        return torrent_write_single_file(NULL, NULL, tsk->tor.pathname,
                                         offset, buffer, buflen);
    } 

    int i;
    int64 invalid_offset = 0, fsize = 0;
    struct single_file *sfile =  tsk->tor.mfile.files;
    for(i = 0; i < tsk->tor.mfile.files_num; i++) {
        invalid_offset = fsize;
        fsize += sfile[i].file_size;
        if(fsize > offset) {
            break;
        }
    }
   
    if(i == tsk->tor.mfile.files_num) {
        LOG_ERROR("Error, can't find the file to write piece!\n");
        return -1;
    }

    int64 writesz = buflen; 
    int64 leftsz = offset + buflen - fsize;
    if(leftsz > 0) {
        writesz = buflen - leftsz;
    }

    if(torrent_write_single_file(tsk->tor.pathname, sfile[i].subdir, sfile[i].pathname,
                                            offset-invalid_offset, buffer, writesz)) {
        return -1; 
    }

    int64 bufoffset = writesz;
    for(i++; leftsz > 0 && i < tsk->tor.mfile.files_num; i++) {
        writesz = leftsz;
        leftsz = leftsz - sfile[i].file_size; 
        if(leftsz > 0) {
            writesz = sfile[i].file_size;
        }

        if(torrent_write_single_file(tsk->tor.pathname, sfile[i].subdir, sfile[i].pathname,
                                            0, buffer+bufoffset, writesz)) {
            return -1;
        }

        bufoffset += writesz;
    }

    return 0;
}

int
torrent_check_downfiles_bitfield(struct torrent_task *tsk)
{
    char *bitmap = tsk->bf.bitmap;
    int idx, havepices = 0, npieces = tsk->bf.npieces;

    LOG_DEBUG("%s local bitfield checking...\n", tsk->tor.pathname);

    char *buffer;
    int buflen;
    for(idx = 0; idx < npieces; idx++) {
        if(torrent_read_piece(tsk, idx, &buffer, &buflen)) {
            continue;
        }

        if(utils_sha1_check(buffer, buflen, &tsk->tor.pieces[idx*20], 20)) {
            free(buffer);
            continue;
        }
        free(buffer);

        havepices++;

        int pidx = idx >> 3; /* idx/8 */
        int bidx = idx & 7;  /* idx%8 */
        unsigned char *byte = (unsigned char *)&bitmap[pidx];
        *byte |= (1 << (7-bidx));        
    }

    LOG_DUMP(bitmap, tsk->bf.nbyte, "%s local bitfield[%d/%d]:",
                                    tsk->tor.pathname, havepices, npieces);

    tsk->leftpieces = npieces - havepices;
    return 0;
}

void*
torrent_io_thread_func(void *thread_ctx)
{
    return NULL;
}

