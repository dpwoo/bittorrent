#ifndef TORRENT_H
#define TORRENT_H

#ifdef __cplusplus
extern "C" {
#endif

 
struct offset {
    char *begin;
    char *end;
};

struct benc_type* get_dict_value_by_key(struct benc_type *bt, const char *str, int val_type);

int torrent_file_parser(char *torfile, struct torrent_file *tor);

int torrent_info_parser(struct torrent_file *tor);

int parser_dict(struct offset *offsz, struct benc_type *bt);

int handle_string_kv(struct benc_type *bt, const char *str, char **setme, int *setmelen);

int handle_int_kv(struct benc_type *bt, const char *str, int *setme);

int torrent_create_downfiles(struct torrent_task *tsk);

int torrent_write_piece(struct torrent_task *tsk, int pieceid, const char *buffer, int buflen);

int torrent_read_piece(struct torrent_task *tsk, int pieceid, char **buffer, int *buflen);

int torrent_check_downfiles_bitfield(struct torrent_task *tsk);

#ifdef __cplusplus
extern "C" }
#endif

#endif
