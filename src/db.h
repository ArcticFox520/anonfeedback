#ifndef ANONFB_DB_H
#define ANONFB_DB_H

#include <pthread.h>
#include <stddef.h>

#include <sqlite3.h>

#include "util.h"

typedef struct {
    sqlite3 *handle;
    pthread_mutex_t lock;
} db_t;

typedef struct {
    char id[UUID_STR_LEN];
    char *content;
    char *category;    /* NULL when unset */
    char *status;
    char *admin_note;  /* NULL when unset */
    char created_at_bucket[ISO8601_UTC_LEN];
    char *updated_at;  /* NULL when never touched */
} feedback_t;

typedef struct {
    feedback_t *items;
    size_t count;
    long total;
} feedback_list_t;

/* Opens the database, applies migrations and hardens the file permissions. */
int db_open(db_t *db, const char *path, const char *migrations_dir);
void db_close(db_t *db);
/* Fails when the feedback table stores any submitter identity column. */
int db_privacy_check(db_t *db);

int db_insert_feedback(db_t *db, const char *id, const char *content,
                       const char *category, const char *created_at_bucket);
/* status == NULL lists every status. */
int db_list_feedback(db_t *db, const char *status, long limit, long offset,
                     feedback_list_t *out);
/* Returns 1 when found, 0 when missing, -1 on error. */
int db_get_feedback(db_t *db, const char *id, feedback_t *out);
/* set_note == 0 leaves admin_note untouched. Returns 1/0/-1. */
int db_update_feedback(db_t *db, const char *id, const char *status, const char *admin_note,
                       int set_note, const char *updated_at);
/* Returns 1 when deleted, 0 when missing, -1 on error. */
int db_delete_feedback(db_t *db, const char *id);

void feedback_free(feedback_t *item);
void feedback_list_free(feedback_list_t *list);

#endif /* ANONFB_DB_H */
