#ifndef SERVER_H
#define SERVER_H

/* ------------ SYSTEM HEADERS ------------ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include "logging.h"
#include "errors.h"



/* ------------ CONSTANTS ------------ */
#define PORT 6000
#define NM_PORT 5000
#define BUFFER 8192
#define DATA_PATH "./data"

/* ------------ PROTOTYPES (utils.c) ------------ */
ssize_t sendall(int sock, const char *buf, size_t len);
void ensure_data_dir();
void register_with_nm();
void format_time_now(char *out, size_t outlen);
void write_meta_simple(const char *filename, int words, int chars, const char *owner);
int read_meta(const char *filename, int *words, int *chars,
              char *owner, size_t owner_len,
              char *timestamp, size_t ts_len);
void update_metadata(const char *filename, const char *owner);
char *read_whole_file(const char *path);
int write_whole_file(const char *path, const char *content);

/* ------------ PROTOTYPES (undo.c) ------------ */
int copy_file_path(const char *src, const char *dst);
int save_undo(const char *filename);
int perform_undo(const char *filename);

/* ------------ PROTOTYPES (sentences.c) ------------ */
char **split_sentences(const char *content, int *out_count);
char *join_sentences(char **sents, int count);
void free_sentences(char **s, int count);
int insert_sentence_at(char ***sents_p, int *count_p, int idx, const char *text);
int insert_word_into_sentence(char **sents, int s_idx, int word_idx, const char *text);

/* ------------ PROTOTYPES (write_handler.c) ------------ */
void handle_write(const char *cmd, int cs);

/* ------------ PROTOTYPES (file_ops.c) ------------ */
void handle_create(const char *fname, int cs);
void handle_read(const char *fname, int cs);
void handle_delete(const char *fname, int cs);
void handle_info(const char *fname, int cs);
void handle_stream(const char *fname, int cs);
void handle_list(int cs);
void handle_addaccess(const char *cmd, int cs);
void handle_remaccess(const char *cmd, int cs);
void handle_view(char *cmd, int cs);
void handle_undo(const char *cmd, int cs);
void handle_createfolder(const char *name, int cs);
void handle_viewfolder(const char *name, int cs);
void handle_move(const char *args, int cs);
void handle_exec(const char *fname, int cs);
void handle_checkpoint(const char *args, int cs);
void handle_viewcheckpoint(const char *args, int cs);
void handle_revert(const char *args, int cs);
void handle_listcheckpoints(const char *args, int cs);


/* Access control helpers */
int load_access(const char *fname, char users[][128], char perms[][8], int max);
void save_access(const char *fname, char users[][128], char perms[][8], int count);

#endif
