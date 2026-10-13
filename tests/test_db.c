#include "test.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "util.h"

static char workspace[256];
static char db_path[512];

static void make_workspace(void)
{
    (void)snprintf(workspace, sizeof(workspace), "/tmp/anonfb_db_XXXXXX");
    CHECK(mkdtemp(workspace) != NULL);
    (void)snprintf(db_path, sizeof(db_path), "%s/feedback.db", workspace);
}

static void test_migrations(const char *migrations_dir)
{
    db_t db;
    sqlite3_stmt *stmt = NULL;

    CHECK(db_open(&db, db_path, migrations_dir) == 0);
    CHECK(db_privacy_check(&db) == 0);
    CHECK(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM schema_migrations", -1, &stmt, NULL) ==
          SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 2);
    (void)sqlite3_finalize(stmt);

    CHECK(sqlite3_prepare_v2(db.handle,
                             "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                             "name='idx_feedback_status_created'",
                             -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 1);
    (void)sqlite3_finalize(stmt);

    /* Reopening must not re-apply migrations. */
    db_close(&db);
    CHECK(db_open(&db, db_path, migrations_dir) == 0);
    CHECK(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM schema_migrations", -1, &stmt, NULL) ==
          SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 2);
    (void)sqlite3_finalize(stmt);
    db_close(&db);
}

static void test_file_permissions(void)
{
    struct stat info;

    CHECK(stat(db_path, &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
}

static void test_crud(const char *migrations_dir)
{
    db_t db;
    feedback_t item;
    feedback_list_t list;
    char id[UUID_STR_LEN];

    CHECK(db_open(&db, db_path, migrations_dir) == 0);

    uuid_v4(id);
    CHECK(db_insert_feedback(&db, id, "first opinion", "bug", "2026-05-01T14:00:00Z") == 0);
    CHECK(db_insert_feedback(&db, "11111111-1111-4111-8111-111111111111", "second opinion", NULL,
                             "2026-05-01T14:00:00Z") == 0);

    CHECK(db_get_feedback(&db, id, &item) == 1);
    CHECK_STR_EQ(item.id, id);
    CHECK_STR_EQ(item.content, "first opinion");
    CHECK_STR_EQ(item.category, "bug");
    CHECK_STR_EQ(item.status, "new");
    CHECK_NULL(item.admin_note);
    CHECK_STR_EQ(item.created_at_bucket, "2026-05-01T14:00:00Z");
    CHECK_NULL(item.updated_at);
    feedback_free(&item);

    CHECK(db_get_feedback(&db, "22222222-2222-4222-8222-222222222222", &item) == 0);

    CHECK(db_list_feedback(&db, NULL, 10, 0, &list) == 0);
    CHECK(list.total == 2);
    CHECK(list.count == 2);
    feedback_list_free(&list);

    CHECK(db_list_feedback(&db, "new", 1, 0, &list) == 0);
    CHECK(list.total == 2);
    CHECK(list.count == 1);
    feedback_list_free(&list);

    CHECK(db_list_feedback(&db, "archived", 10, 0, &list) == 0);
    CHECK(list.total == 0);
    CHECK(list.count == 0);
    feedback_list_free(&list);

    CHECK(db_update_feedback(&db, id, "read", NULL, 0, "2026-05-01T15:00:00Z") == 1);
    CHECK(db_get_feedback(&db, id, &item) == 1);
    CHECK_STR_EQ(item.status, "read");
    CHECK_NULL(item.admin_note);
    CHECK_STR_EQ(item.updated_at, "2026-05-01T15:00:00Z");
    feedback_free(&item);

    CHECK(db_update_feedback(&db, id, NULL, "handled", 1, "2026-05-01T16:00:00Z") == 1);
    CHECK(db_get_feedback(&db, id, &item) == 1);
    CHECK_STR_EQ(item.status, "read");
    CHECK_STR_EQ(item.admin_note, "handled");
    feedback_free(&item);

    CHECK(db_update_feedback(&db, id, "archived", NULL, 1, "2026-05-01T17:00:00Z") == 1);
    CHECK(db_get_feedback(&db, id, &item) == 1);
    CHECK_STR_EQ(item.status, "archived");
    CHECK_NULL(item.admin_note);
    feedback_free(&item);

    CHECK(db_update_feedback(&db, "33333333-3333-4333-8333-333333333333", "read", NULL, 0,
                             "2026-05-01T17:00:00Z") == 0);

    CHECK(db_delete_feedback(&db, id) == 1);
    CHECK(db_delete_feedback(&db, id) == 0);
    CHECK(db_get_feedback(&db, id, &item) == 0);

    db_close(&db);
}

static void test_constraints_and_injection(const char *migrations_dir)
{
    db_t db;
    feedback_t item;
    feedback_list_t list;
    char *long_content = xmalloc(5001 + 1);
    char id[UUID_STR_LEN];
    const char *injection = "'); DROP TABLE feedback; --";

    CHECK(db_open(&db, db_path, migrations_dir) == 0);
    memset(long_content, 'a', 5001);
    long_content[5001] = '\0';

    uuid_v4(id);
    CHECK(db_insert_feedback(&db, id, long_content, "bug", "2026-05-01T14:00:00Z") == -1);
    free(long_content);

    /* Invalid enum values are rejected by the schema. */
    CHECK(db_insert_feedback(&db, id, "body", "invalid", "2026-05-01T14:00:00Z") == -1);
    CHECK(db_insert_feedback(&db, id, "body", "bug", "not-a-bucket") == 0);

    /* Parameter binding keeps hostile content inert. */
    uuid_v4(id);
    CHECK(db_insert_feedback(&db, id, injection, NULL, "2026-05-01T14:00:00Z") == 0);
    CHECK(db_get_feedback(&db, id, &item) == 1);
    CHECK_STR_EQ(item.content, injection);
    feedback_free(&item);
    CHECK(db_list_feedback(&db, NULL, 10, 0, &list) == 0);
    CHECK(list.total >= 1);
    feedback_list_free(&list);

    db_close(&db);
}

static void test_privacy_check_rejects_identity_column(void)
{
    db_t raw;
    sqlite3 *handle = NULL;

    CHECK(sqlite3_open_v2(":memory:", &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) ==
          SQLITE_OK);
    CHECK(sqlite3_exec(handle, "CREATE TABLE feedback (id TEXT, ip TEXT)", NULL, NULL, NULL) ==
          SQLITE_OK);
    memset(&raw, 0, sizeof(raw));
    raw.handle = handle;
    CHECK(pthread_mutex_init(&raw.lock, NULL) == 0);
    CHECK(db_privacy_check(&raw) == -1);
    (void)pthread_mutex_destroy(&raw.lock);
    (void)sqlite3_close(handle);
}

int main(int argc, char **argv)
{
    const char *migrations_dir = argc > 1 ? argv[1] : "migrations";

    make_workspace();
    CHECK(workspace[0] != '\0');
    test_migrations(migrations_dir);
    test_file_permissions();
    test_crud(migrations_dir);
    test_constraints_and_injection(migrations_dir);
    test_privacy_check_rejects_identity_column();
    TEST_REPORT();
}
