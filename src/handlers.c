#include "handlers.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "log.h"
#include "util.h"

#define CONTENT_MAX_CHARS 5000u
#define NOTE_MAX_CHARS 2000u
#define PAGE_DEFAULT_LIMIT 20u
#define PAGE_MAX_LIMIT 100u
#define PAGE_MAX_OFFSET 10000000u

static cJSON *feedback_to_json(const feedback_t *item)
{
    cJSON *object = cJSON_CreateObject();

    if (object == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(object, "id", item->id);
    cJSON_AddStringToObject(object, "content", item->content != NULL ? item->content : "");
    if (item->category != NULL) {
        cJSON_AddStringToObject(object, "category", item->category);
    } else {
        cJSON_AddNullToObject(object, "category");
    }
    cJSON_AddStringToObject(object, "status", item->status != NULL ? item->status : "new");
    if (item->admin_note != NULL) {
        cJSON_AddStringToObject(object, "admin_note", item->admin_note);
    } else {
        cJSON_AddNullToObject(object, "admin_note");
    }
    cJSON_AddStringToObject(object, "created_at_bucket", item->created_at_bucket);
    if (item->updated_at != NULL) {
        cJSON_AddStringToObject(object, "updated_at", item->updated_at);
    } else {
        cJSON_AddNullToObject(object, "updated_at");
    }
    return object;
}

enum MHD_Result handle_health(request_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        return MHD_NO;
    }
    cJSON_AddStringToObject(root, "status", "ok");
    return http_reply_json(req, MHD_HTTP_OK, root);
}

enum MHD_Result handle_meta(request_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *categories = cJSON_CreateArray();

    if (root == NULL || categories == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(categories);
        return MHD_NO;
    }
    cJSON_AddItemToArray(categories, cJSON_CreateString("bug"));
    cJSON_AddItemToArray(categories, cJSON_CreateString("suggestion"));
    cJSON_AddItemToArray(categories, cJSON_CreateString("complaint"));
    cJSON_AddItemToArray(categories, cJSON_CreateString("other"));
    cJSON_AddItemToObject(root, "categories", categories);
    cJSON_AddNumberToObject(root, "content_max_chars", CONTENT_MAX_CHARS);
    return http_reply_json(req, MHD_HTTP_OK, root);
}

enum MHD_Result handle_submit_feedback(request_t *req)
{
    cJSON *root = NULL;
    cJSON *content_item;
    cJSON *category_item;
    char *content = NULL;
    char *category_raw = NULL;
    const char *category = NULL;
    char id[UUID_STR_LEN];
    char bucket[ISO8601_UTC_LEN];
    size_t content_chars;
    enum MHD_Result result;
    cJSON *response;

    if (!ratelimit_allow(&req->app->submit_limiter, req->peer)) {
        return http_reply_error(req, MHD_HTTP_TOO_MANY_REQUESTS, "RATE_LIMITED",
                                "提交过于频繁，请稍后再试");
    }
    if (!http_body_is_json(req)) {
        return http_reply_error(req, MHD_HTTP_UNSUPPORTED_MEDIA_TYPE, "UNSUPPORTED_MEDIA_TYPE",
                                "请求体必须为 application/json");
    }
    if (req->body_len == 0) {
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_JSON", "请求体不是合法 JSON");
    }
    root = cJSON_ParseWithLength(req->body, req->body_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_JSON", "请求体不是合法 JSON");
    }

    content_item = cJSON_GetObjectItemCaseSensitive(root, "content");
    if (!cJSON_IsString(content_item) || content_item->valuestring == NULL) {
        cJSON_Delete(root);
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_CONTENT", "content 字段必填");
    }
    content = str_trim_dup(content_item->valuestring);
    content_chars = strlen(content);
    if (!utf8_valid(content, content_chars)) {
        free(content);
        cJSON_Delete(root);
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_CONTENT", "content 必须是合法 UTF-8");
    }
    content_chars = utf8_count(content, content_chars);
    if (content_chars < 1 || content_chars > CONTENT_MAX_CHARS) {
        free(content);
        cJSON_Delete(root);
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_CONTENT",
                                "content 长度必须为 1-5000 字符");
    }

    category_item = cJSON_GetObjectItemCaseSensitive(root, "category");
    if (category_item != NULL && !cJSON_IsNull(category_item)) {
        if (!cJSON_IsString(category_item) || category_item->valuestring == NULL) {
            free(content);
            cJSON_Delete(root);
            return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_CATEGORY",
                                    "category 取值非法");
        }
        category_raw = str_trim_dup(category_item->valuestring);
        category = category_canonical(category_raw);
        if (category_raw[0] != '\0' && category == NULL) {
            free(category_raw);
            free(content);
            cJSON_Delete(root);
            return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_CATEGORY",
                                    "category 取值非法");
        }
    }
    free(category_raw);
    cJSON_Delete(root);

    uuid_v4(id);
    hour_bucket_utc(time(NULL), bucket);
    if (db_insert_feedback(req->app->db, id, content, category, bucket) != 0) {
        free(content);
        return http_reply_error(req, MHD_HTTP_INTERNAL_SERVER_ERROR, "INTERNAL_ERROR",
                                "服务器内部错误");
    }
    log_info("feedback stored");
    free(content);

    response = cJSON_CreateObject();
    if (response == NULL) {
        return MHD_NO;
    }
    cJSON_AddStringToObject(response, "id", id);
    cJSON_AddStringToObject(response, "status", "new");
    if (category != NULL) {
        cJSON_AddStringToObject(response, "category", category);
    } else {
        cJSON_AddNullToObject(response, "category");
    }
    cJSON_AddStringToObject(response, "created_at_bucket", bucket);
    result = http_reply_json(req, MHD_HTTP_CREATED, response);
    return result;
}

enum MHD_Result handle_admin_list(request_t *req)
{
    const char *status = MHD_lookup_connection_value(req->conn, MHD_GET_ARGUMENT_KIND, "status");
    const char *limit_text = MHD_lookup_connection_value(req->conn, MHD_GET_ARGUMENT_KIND, "limit");
    const char *offset_text =
        MHD_lookup_connection_value(req->conn, MHD_GET_ARGUMENT_KIND, "offset");
    long limit = (long)PAGE_DEFAULT_LIMIT;
    long offset = 0;
    feedback_list_t list;
    cJSON *root;
    cJSON *items;
    size_t i;

    if (status != NULL && status[0] == '\0') {
        status = NULL;
    }
    if (status != NULL && !status_valid(status)) {
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_STATUS", "status 取值非法");
    }
    if (limit_text != NULL && !parse_int_range(limit_text, 1, (long)PAGE_MAX_LIMIT, &limit)) {
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_PAGINATION", "limit 取值非法");
    }
    if (offset_text != NULL && !parse_int_range(offset_text, 0, (long)PAGE_MAX_OFFSET, &offset)) {
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_PAGINATION", "offset 取值非法");
    }
    if (db_list_feedback(req->app->db, status, limit, offset, &list) != 0) {
        return http_reply_error(req, MHD_HTTP_INTERNAL_SERVER_ERROR, "INTERNAL_ERROR",
                                "服务器内部错误");
    }

    root = cJSON_CreateObject();
    items = cJSON_CreateArray();
    if (root == NULL || items == NULL) {
        feedback_list_free(&list);
        cJSON_Delete(root);
        cJSON_Delete(items);
        return MHD_NO;
    }
    for (i = 0; i < list.count; i++) {
        cJSON *item = feedback_to_json(&list.items[i]);

        if (item != NULL) {
            cJSON_AddItemToArray(items, item);
        }
    }
    cJSON_AddItemToObject(root, "items", items);
    cJSON_AddNumberToObject(root, "total", (double)list.total);
    cJSON_AddNumberToObject(root, "limit", (double)limit);
    cJSON_AddNumberToObject(root, "offset", (double)offset);
    feedback_list_free(&list);
    return http_reply_json(req, MHD_HTTP_OK, root);
}

enum MHD_Result handle_admin_get(request_t *req, const char *id)
{
    feedback_t item;
    int found = db_get_feedback(req->app->db, id, &item);

    if (found < 0) {
        return http_reply_error(req, MHD_HTTP_INTERNAL_SERVER_ERROR, "INTERNAL_ERROR",
                                "服务器内部错误");
    }
    if (found == 0) {
        return http_reply_error(req, MHD_HTTP_NOT_FOUND, "NOT_FOUND", "意见不存在");
    }
    {
        cJSON *body = feedback_to_json(&item);

        feedback_free(&item);
        if (body == NULL) {
            return MHD_NO;
        }
        return http_reply_json(req, MHD_HTTP_OK, body);
    }
}

enum MHD_Result handle_admin_update(request_t *req, const char *id)
{
    cJSON *root;
    cJSON *status_item;
    cJSON *note_item;
    const char *status = NULL;
    char *status_value = NULL;
    char *note = NULL;
    char *note_trimmed = NULL;
    int set_note;
    char updated_at[ISO8601_UTC_LEN];
    int updated;
    feedback_t item;

    if (!http_body_is_json(req)) {
        return http_reply_error(req, MHD_HTTP_UNSUPPORTED_MEDIA_TYPE, "UNSUPPORTED_MEDIA_TYPE",
                                "请求体必须为 application/json");
    }
    if (req->body_len == 0) {
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_JSON", "请求体不是合法 JSON");
    }
    root = cJSON_ParseWithLength(req->body, req->body_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_JSON", "请求体不是合法 JSON");
    }

    status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
    note_item = cJSON_GetObjectItemCaseSensitive(root, "admin_note");
    set_note = note_item != NULL;

    if (status_item == NULL && note_item == NULL) {
        cJSON_Delete(root);
        return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "EMPTY_UPDATE", "没有可更新的字段");
    }
    if (status_item != NULL) {
        if (!cJSON_IsString(status_item) || status_item->valuestring == NULL ||
            !status_valid(status_item->valuestring)) {
            cJSON_Delete(root);
            return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_STATUS", "status 取值非法");
        }
        status = status_item->valuestring;
    }
    if (set_note && !cJSON_IsNull(note_item)) {
        size_t note_len;

        if (!cJSON_IsString(note_item) || note_item->valuestring == NULL) {
            cJSON_Delete(root);
            return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_NOTE", "admin_note 取值非法");
        }
        note = str_trim_dup(note_item->valuestring);
        note_len = strlen(note);
        if (!utf8_valid(note, note_len) || utf8_count(note, note_len) > NOTE_MAX_CHARS) {
            free(note);
            cJSON_Delete(root);
            return http_reply_error(req, MHD_HTTP_BAD_REQUEST, "INVALID_NOTE",
                                    "admin_note 长度不能超过 2000 字符");
        }
        note_trimmed = note[0] != '\0' ? note : NULL;
    }
    if (status != NULL) {
        status_value = xstrdup(status);
    }
    cJSON_Delete(root);

    iso8601_utc(time(NULL), updated_at);
    updated = db_update_feedback(req->app->db, id, status_value, note_trimmed, set_note, updated_at);
    free(note);
    free(status_value);
    if (updated < 0) {
        return http_reply_error(req, MHD_HTTP_INTERNAL_SERVER_ERROR, "INTERNAL_ERROR",
                                "服务器内部错误");
    }
    if (updated == 0) {
        return http_reply_error(req, MHD_HTTP_NOT_FOUND, "NOT_FOUND", "意见不存在");
    }
    if (db_get_feedback(req->app->db, id, &item) != 1) {
        return http_reply_error(req, MHD_HTTP_INTERNAL_SERVER_ERROR, "INTERNAL_ERROR",
                                "服务器内部错误");
    }
    {
        cJSON *body = feedback_to_json(&item);

        feedback_free(&item);
        if (body == NULL) {
            return MHD_NO;
        }
        return http_reply_json(req, MHD_HTTP_OK, body);
    }
}

enum MHD_Result handle_admin_delete(request_t *req, const char *id)
{
    int deleted = db_delete_feedback(req->app->db, id);

    if (deleted < 0) {
        return http_reply_error(req, MHD_HTTP_INTERNAL_SERVER_ERROR, "INTERNAL_ERROR",
                                "服务器内部错误");
    }
    if (deleted == 0) {
        return http_reply_error(req, MHD_HTTP_NOT_FOUND, "NOT_FOUND", "意见不存在");
    }
    log_info("feedback deleted");
    return http_reply_empty(req, MHD_HTTP_NO_CONTENT);
}
