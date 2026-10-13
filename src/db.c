#include "db.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "log.h"

#define MIGRATION_FILE_MAX_BYTES (1024u * 1024u)

static int lock_db(db_t *db)
{
    return pthread_mutex_lock(&db->lock);
}

static void unlock_db(db_t *db)
{
    (void)pthread_mutex_unlock(&db->lock);
}

static int exec_sql(sqlite3 *handle, const char *sql)
{
    char *message = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &message);

    if (rc != SQLITE_OK) {
        log_error("sqlite statement failed: %s", message != NULL ? message : "unknown error");
        sqlite3_free(message);
        return -1;
    }
    return 0;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *buffer;
    long size;
    size_t read_bytes;

    if (file == NULL) {
        log_error("cannot open migration file");
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || (unsigned long)size > MIGRATION_FILE_MAX_BYTES) {
        log_error("migration file is too large");
        (void)fclose(file);
        return NULL;
    }
    rewind(file);
    buffer = xmalloc((size_t)size + 1);
    read_bytes = fread(buffer, 1, (size_t)size, file);
    (void)fclose(file);
    if (read_bytes != (size_t)size) {
        free(buffer);
        return NULL;
    }
    buffer[size] = '\0';
    return buffer;
}

static int ensure_parent_dir(const char *path)
{
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    int result = 0;

    if (slash != NULL && slash != copy) {
        *slash = '\0';
        if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
            log_error("cannot create database directory");
            result = -1;
        }
    }
    free(copy);
    return result;
}

static int compare_names(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;

    return strcmp(*a, *b);
}

static int migration_applied(db_t *db, const char *name, int *applied)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    *applied = 0;
    rc = sqlite3_prepare_v2(db->handle,
                            "SELECT 1 FROM schema_migrations WHERE version = ?1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    (void)sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *applied = 1;
    } else if (rc != SQLITE_DONE) {
        (void)sqlite3_finalize(stmt);
        return -1;
    }
    (void)sqlite3_finalize(stmt);
    return 0;
}

static int record_migration(db_t *db, const char *name, const char *applied_at)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db->handle,
                            "INSERT INTO schema_migrations (version, applied_at) VALUES (?1, ?2)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    (void)sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(stmt, 2, applied_at, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    (void)sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int apply_migration(db_t *db, const char *dir, const char *name)
{
    char path[4096];
    char *sql;
    char applied_at[ISO8601_UTC_LEN];
    int result = -1;

    if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, name) >= sizeof(path)) {
        log_error("migration path is too long");
        return -1;
    }
    sql = read_file(path);
    if (sql == NULL) {
        return -1;
    }
    iso8601_utc(time(NULL), applied_at);

    if (exec_sql(db->handle, "BEGIN IMMEDIATE") != 0) {
        free(sql);
        return -1;
    }
    if (exec_sql(db->handle, sql) == 0 && record_migration(db, name, applied_at) == 0) {
        if (exec_sql(db->handle, "COMMIT") == 0) {
            result = 0;
        }
    }
    if (result != 0) {
        log_error("migration failed");
        (void)exec_sql(db->handle, "ROLLBACK");
    }
    free(sql);
    return result;
}

static int run_migrations(db_t *db, const char *dir)
{
    DIR *handle = opendir(dir);
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t i;
    int result = 0;

    if (handle == NULL) {
        log_error("cannot open migrations directory");
        return -1;
    }
    while ((entry = readdir(handle)) != NULL) {
        size_t len = strlen(entry->d_name);

        if (len < 5 || strcmp(entry->d_name + len - 4, ".sql") != 0) {
            continue;
        }
        if (count == capacity) {
            capacity = capacity == 0 ? 16 : capacity * 2;
            names = xrealloc(names, capacity * sizeof(*names));
        }
        names[count++] = xstrdup(entry->d_name);
    }
    (void)closedir(handle);

    qsort(names, count, sizeof(*names), compare_names);

    for (i = 0; i < count && result == 0; i++) {
        int applied = 0;

        if (migration_applied(db, names[i], &applied) != 0) {
            log_error("cannot read migration state");
            result = -1;
            break;
        }
        if (!applied && apply_migration(db, dir, names[i]) != 0) {
            result = -1;
            break;
        }
    }
    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
    return result;
}

/* Fails when a stored column could identify a submitter. */
int db_privacy_check(db_t *db)
{
    static const char *const forbidden[] = {"ip", "ip_address", "user_agent", "cookie",
                                            "referer", "referrer", "session", "session_id",
                                            "device", "device_id", "fingerprint", "token"};
    sqlite3_stmt *stmt = NULL;
    int result = 0;
    int rc;

    rc = sqlite3_prepare_v2(db->handle, "PRAGMA table_info(feedback)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *column = sqlite3_column_text(stmt, 1);
        size_t i;

        for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
            if (column != NULL && strcmp((const char *)column, forbidden[i]) == 0) {
                log_error("privacy check failed: forbidden column in feedback table");
                result = -1;
            }
        }
    }
    (void)sqlite3_finalize(stmt);
    return result;
}

int db_open(db_t *db, const char *path, const char *migrations_dir)
{
    memset(db, 0, sizeof(*db));
    if (pthread_mutex_init(&db->lock, NULL) != 0) {
        log_error("database mutex init failed");
        return -1;
    }
    if (ensure_parent_dir(path) != 0) {
        (void)pthread_mutex_destroy(&db->lock);
        return -1;
    }
    if (sqlite3_open_v2(path, &db->handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        log_error("cannot open database");
        (void)pthread_mutex_destroy(&db->lock);
        return -1;
    }
    sqlite3_busy_timeout(db->handle, 5000);
    if (exec_sql(db->handle, "PRAGMA journal_mode=WAL") != 0 ||
        exec_sql(db->handle, "PRAGMA synchronous=NORMAL") != 0 ||
        exec_sql(db->handle, "PRAGMA foreign_keys=ON") != 0 ||
        exec_sql(db->handle, "CREATE TABLE IF NOT EXISTS schema_migrations ("
                             "version TEXT PRIMARY KEY NOT NULL, applied_at TEXT NOT NULL)") != 0 ||
        run_migrations(db, migrations_dir) != 0 || db_privacy_check(db) != 0) {
        db_close(db);
        return -1;
    }
    (void)chmod(path, 0600);
    return 0;
}

void db_close(db_t *db)
{
    if (db == NULL) {
        return;
    }
    if (db->handle != NULL) {
        (void)sqlite3_close(db->handle);
        db->handle = NULL;
    }
    (void)pthread_mutex_destroy(&db->lock);
}

static void copy_fixed(char *dest, size_t dest_size, const char *src)
{
    size_t len = src != NULL ? strlen(src) : 0;

    if (len >= dest_size) {
        len = dest_size - 1;
    }
    if (len > 0 && src != NULL) {
        memcpy(dest, src, len);
    }
    dest[len] = '\0';
}

static char *column_text_or_null(sqlite3_stmt *stmt, int index)
{
    const unsigned char *value = sqlite3_column_text(stmt, index);

    return value != NULL ? xstrdup((const char *)value) : NULL;
}

static void read_row(sqlite3_stmt *stmt, feedback_t *out)
{
    memset(out, 0, sizeof(*out));
    copy_fixed(out->id, sizeof(out->id), (const char *)sqlite3_column_text(stmt, 0));
    out->content = column_text_or_null(stmt, 1);
    out->category = column_text_or_null(stmt, 2);
    out->status = column_text_or_null(stmt, 3);
    out->admin_note = column_text_or_null(stmt, 4);
    copy_fixed(out->created_at_bucket, sizeof(out->created_at_bucket),
               (const char *)sqlite3_column_text(stmt, 5));
    out->updated_at = column_text_or_null(stmt, 6);
}

int db_insert_feedback(db_t *db, const char *id, const char *content, const char *category,
                       const char *created_at_bucket)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    int result = -1;

    /* Reject any timestamp finer than the UTC hour. */
    if (!hour_bucket_valid(created_at_bucket)) {
        log_error("refused to store a timestamp that is not an hour bucket");
        return -1;
    }
    (void)lock_db(db);
    rc = sqlite3_prepare_v2(db->handle,
                            "INSERT INTO feedback (id, content, category, status, admin_note, "
                            "created_at_bucket, updated_at) "
                            "VALUES (?1, ?2, ?3, 'new', NULL, ?4, NULL)",
                            -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        (void)sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
        (void)sqlite3_bind_text(stmt, 2, content, -1, SQLITE_STATIC);
        if (category != NULL) {
            (void)sqlite3_bind_text(stmt, 3, category, -1, SQLITE_STATIC);
        } else {
            (void)sqlite3_bind_null(stmt, 3);
        }
        (void)sqlite3_bind_text(stmt, 4, created_at_bucket, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            result = 0;
        } else {
            log_error("feedback insert failed");
        }
        (void)sqlite3_finalize(stmt);
    } else {
        log_error("feedback insert statement failed");
    }
    unlock_db(db);
    return result;
}

int db_list_feedback(db_t *db, const char *status, long limit, long offset,
                     feedback_list_t *out)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    int result = -1;

    memset(out, 0, sizeof(*out));
    (void)lock_db(db);

    rc = sqlite3_prepare_v2(db->handle,
                            "SELECT COUNT(*) FROM feedback WHERE (?1 IS NULL OR status = ?1)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        goto done;
    }
    if (status != NULL) {
        (void)sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    } else {
        (void)sqlite3_bind_null(stmt, 1);
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->total = (long)sqlite3_column_int64(stmt, 0);
    } else {
        (void)sqlite3_finalize(stmt);
        goto done;
    }
    (void)sqlite3_finalize(stmt);
    stmt = NULL;

    rc = sqlite3_prepare_v2(db->handle,
                            "SELECT id, content, category, status, admin_note, created_at_bucket, "
                            "updated_at FROM feedback WHERE (?1 IS NULL OR status = ?1) "
                            "ORDER BY created_at_bucket DESC, rowid DESC LIMIT ?2 OFFSET ?3",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        goto done;
    }
    if (status != NULL) {
        (void)sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    } else {
        (void)sqlite3_bind_null(stmt, 1);
    }
    (void)sqlite3_bind_int64(stmt, 2, limit);
    (void)sqlite3_bind_int64(stmt, 3, offset);

    out->items = xcalloc((size_t)limit, sizeof(*out->items));
    for (;;) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            if (out->count < (size_t)limit) {
                read_row(stmt, &out->items[out->count]);
                out->count++;
            }
            continue;
        }
        if (rc == SQLITE_DONE) {
            result = 0;
        } else {
            log_error("feedback list failed");
        }
        break;
    }

done:
    if (stmt != NULL) {
        (void)sqlite3_finalize(stmt);
    }
    if (result != 0) {
        feedback_list_free(out);
    }
    unlock_db(db);
    return result;
}

int db_get_feedback(db_t *db, const char *id, feedback_t *out)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    int result = -1;

    memset(out, 0, sizeof(*out));
    (void)lock_db(db);
    rc = sqlite3_prepare_v2(db->handle,
                            "SELECT id, content, category, status, admin_note, created_at_bucket, "
                            "updated_at FROM feedback WHERE id = ?1",
                            -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        (void)sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            read_row(stmt, out);
            result = 1;
        } else if (rc == SQLITE_DONE) {
            result = 0;
        }
    }
    if (stmt != NULL) {
        (void)sqlite3_finalize(stmt);
    }
    unlock_db(db);
    return result;
}

int db_update_feedback(db_t *db, const char *id, const char *status, const char *admin_note,
                       int set_note, const char *updated_at)
{
    const char *sql;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int result = -1;

    if (status != NULL && set_note) {
        sql = "UPDATE feedback SET status = ?1, admin_note = ?2, updated_at = ?3 WHERE id = ?4";
    } else if (status != NULL) {
        sql = "UPDATE feedback SET status = ?1, updated_at = ?2 WHERE id = ?3";
    } else if (set_note) {
        sql = "UPDATE feedback SET admin_note = ?1, updated_at = ?2 WHERE id = ?3";
    } else {
        return -1;
    }

    (void)lock_db(db);
    rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        if (status != NULL && set_note) {
            (void)sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
            if (admin_note != NULL) {
                (void)sqlite3_bind_text(stmt, 2, admin_note, -1, SQLITE_STATIC);
            } else {
                (void)sqlite3_bind_null(stmt, 2);
            }
            (void)sqlite3_bind_text(stmt, 3, updated_at, -1, SQLITE_STATIC);
            (void)sqlite3_bind_text(stmt, 4, id, -1, SQLITE_STATIC);
        } else if (status != NULL) {
            (void)sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
            (void)sqlite3_bind_text(stmt, 2, updated_at, -1, SQLITE_STATIC);
            (void)sqlite3_bind_text(stmt, 3, id, -1, SQLITE_STATIC);
        } else {
            if (admin_note != NULL) {
                (void)sqlite3_bind_text(stmt, 1, admin_note, -1, SQLITE_STATIC);
            } else {
                (void)sqlite3_bind_null(stmt, 1);
            }
            (void)sqlite3_bind_text(stmt, 2, updated_at, -1, SQLITE_STATIC);
            (void)sqlite3_bind_text(stmt, 3, id, -1, SQLITE_STATIC);
        }
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            result = sqlite3_changes(db->handle) > 0 ? 1 : 0;
        } else {
            log_error("feedback update failed");
        }
        (void)sqlite3_finalize(stmt);
    }
    unlock_db(db);
    return result;
}

int db_delete_feedback(db_t *db, const char *id)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    int result = -1;

    (void)lock_db(db);
    rc = sqlite3_prepare_v2(db->handle, "DELETE FROM feedback WHERE id = ?1", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        (void)sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            result = sqlite3_changes(db->handle) > 0 ? 1 : 0;
        } else {
            log_error("feedback delete failed");
        }
        (void)sqlite3_finalize(stmt);
    }
    unlock_db(db);
    return result;
}

void feedback_free(feedback_t *item)
{
    if (item == NULL) {
        return;
    }
    free(item->content);
    free(item->category);
    free(item->status);
    free(item->admin_note);
    free(item->updated_at);
    memset(item, 0, sizeof(*item));
}

void feedback_list_free(feedback_list_t *list)
{
    size_t i;

    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->count; i++) {
        feedback_free(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->total = 0;
}
