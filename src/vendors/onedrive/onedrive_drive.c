/*
 * Copyright (c) 2019 Elastos Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif

#include <crystal.h>
#include <cjson/cJSON.h>

#include "hive_error.h"
#include "onedrive_misc.h"
#include "onedrive_constants.h"
#include "http_client.h"
#include "http_status.h"

#define ARGV(args, index) (((void **)(args))[index])

typedef struct OneDriveDrive {
    HiveDrive base;
    oauth_token_t *token;
    char tmp_template[PATH_MAX];
} OneDriveDrive;

#define DECODE_INFO_FIELD(json, name, field) do { \
        int rc; \
        rc = decode_info_field(json, name, field, sizeof(field)); \
        if (rc < 0) { \
            vlogE("OneDriveDrive: missing %s json object.", name); \
            cJSON_Delete(json); \
            return rc; \
        } \
    } while(0)

static
int onedrive_decode_drive_info(const char *info_str, HiveDriveInfo *info)
{
    cJSON *json;

    assert(info_str);
    assert(info);

    json = cJSON_Parse(info_str);
    if (!json) {
        vlogE("OneDriveDrive: bad json format.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    DECODE_INFO_FIELD(json, "id", info->driveid);

    cJSON_Delete(json);
    return 0;
}

static
int onedrive_drive_get_info(HiveDrive *base, HiveDriveInfo *info)
{
    OneDriveDrive *drive = (OneDriveDrive *)base;
    http_client_t *httpc;
    char *p = NULL;
    long resp_code = 0;
    int rc;

    assert(drive);
    assert(drive->token);
    assert(info);

    rc = oauth_token_check_expire(drive->token);
    if (rc < 0) {
        vlogE("OneDriveDrive: checking access token expired error.");
        return rc;
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("OneDriveDrive: failed to create http client.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    http_client_set_url(httpc, MY_DRIVE);
    http_client_set_method(httpc, HTTP_METHOD_GET);
    http_client_set_header(httpc, "Authorization", get_bearer_token(drive->token));
    http_client_enable_response_body(httpc);

    rc = http_client_request(httpc);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("OneDriveDrive: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("OneDriveDrive: failed to get http response code.");
        goto error_exit;
    }

    if (resp_code == HttpStatus_Unauthorized) {
        vlogE("OneDriveDrive: access token expired.");
        oauth_token_set_expired(drive->token);
        rc = HIVE_GENERAL_ERROR(HIVEERR_TRY_AGAIN);
        goto error_exit;
    }

    if (resp_code != HttpStatus_OK) {
        vlogE("OneDriveDrive: error from http response (%d).", resp_code);
        rc = HIVE_HTTP_STATUS_ERROR(resp_code);
        goto error_exit;
    }

    p = http_client_move_response_body(httpc, NULL);
    http_client_close(httpc);

    if (!p) {
        vlogE("OneDriveDrive: failed to get response body.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    rc = onedrive_decode_drive_info(p, info);
    free(p);

    if (rc < 0)
        vlogE("OneDriveDrive: failed to decode drive info.");

    return rc;

error_exit:
    http_client_close(httpc);
    return rc;
}

static
int onedrive_decode_file_info(const char *info_str, HiveFileInfo *info)
{
    cJSON *json;
    cJSON *file;
    cJSON *dir;
    cJSON *size;

    assert(info_str);
    assert(info);

    json = cJSON_Parse(info_str);
    if (!json) {
        vlogE("OneDriveDrive: bad json format.");
        return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
    }

    DECODE_INFO_FIELD(json, "cTag", info->fileid);

    file = cJSON_GetObjectItemCaseSensitive(json, "file");
    dir  = cJSON_GetObjectItemCaseSensitive(json, "folder");
    if ((file && dir) || (!file && !dir)) {
        vlogE("OneDriveDrive: problem with file or folder json object.");
        cJSON_Delete(json);
        return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
    }

    if (file)
        strcpy(info->type, "file");
    else
        strcpy(info->type, "directory");

    size = cJSON_GetObjectItemCaseSensitive(json, "size");
    if (!size || !cJSON_IsNumber(size)) {
        vlogE("OneDriveDrive: missing size json object.");
        cJSON_Delete(json);
        return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
    }

    info->size = (size_t)size->valuedouble;

    cJSON_Delete(json);
    return 0;
}

static
int onedrive_drive_stat_file(HiveDrive *base, const char *path, HiveFileInfo *info)
{
    OneDriveDrive *drive = (OneDriveDrive *)base;
    http_client_t *httpc;
    char url[MAX_URL_LEN] = {0};
    char *p;
    long resp_code = 0;
    int rc;

    assert(drive);
    assert(drive->token);
    assert(path);
    assert(*path == '/');
    assert(info);

    rc = oauth_token_check_expire(drive->token);
    if (rc < 0) {
        vlogE("OneDriveDrive: checking access token expired error.");
        return rc;
    }

    if (strlen(path) >= MAX_URL_PARAM_LEN) {
        vlogE("OneDriveDrive: path too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS);
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("OneDriveDrive: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    if (!strcmp(path, "/"))
        sprintf(url, "%s/root", MY_DRIVE);
    else
        sprintf(url, "%s/root:%s", MY_DRIVE, path);

    http_client_set_url(httpc, url);
    http_client_set_method(httpc, HTTP_METHOD_GET);
    http_client_set_header(httpc, "Authorization", get_bearer_token(drive->token));
    http_client_enable_response_body(httpc);

    rc = http_client_request(httpc);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("OneDriveDrive: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("OneDriveDrive: failed to get http response code.");
        goto error_exit;
    }

    if (resp_code == HttpStatus_Unauthorized) {
        vlogE("OneDriveDrive: access token expired.");
        oauth_token_set_expired(drive->token);
        rc = HIVE_GENERAL_ERROR(HIVEERR_TRY_AGAIN);
        goto error_exit;
    }

    if (resp_code != HttpStatus_OK) {
        vlogE("OneDriveDrive: error from http response (%d).", resp_code);
        rc = HIVE_HTTP_STATUS_ERROR(resp_code);
        goto error_exit;
    }

    p = http_client_move_response_body(httpc, NULL);
    http_client_close(httpc);

    if (!p) {
        vlogE("OneDriveDrive: failed to get response body.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    rc = onedrive_decode_file_info(p, info);
    free(p);

    return rc;

error_exit:
    http_client_close(httpc);
    return rc;
}

static
int merge_array(cJSON *sub, cJSON *array)
{
    cJSON *item;
    size_t sub_sz = cJSON_GetArraySize(sub);
    size_t i;

    for (i = 0; i < sub_sz; ++i) {
        cJSON *name;
        cJSON *file;
        cJSON *folder;

        item = cJSON_GetArrayItem(sub, 0);

        name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!name || !cJSON_IsString(name) || !name->valuestring || !*name->valuestring) {
            vlogE("OneDriveDrive: missing name json object.");
            return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
        }

        file = cJSON_GetObjectItemCaseSensitive(item, "file");
        folder = cJSON_GetObjectItemCaseSensitive(item, "folder");
        if ((file && folder) || (!file && !folder)) {
            vlogE("OneDriveDrive: bad json format for file and folder json object.");
            return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
        }

        if (file && !cJSON_IsObject(file)) {
            vlogE("OneDriveDrive: bad format for file json object.");
            return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
        }

        if (folder && !cJSON_IsObject(folder)) {
            vlogE("OneDriveDrive: bad format for folder json object.");
            return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
        }

        cJSON_AddItemToArray(array, cJSON_DetachItemFromArray(sub, 0));
    }

    return 0;
}

static
void notify_user_files(cJSON *array, HiveFilesIterateCallback *callback,
                       void *context)
{
    cJSON *item;

    cJSON_ArrayForEach(item, array) {
        cJSON *name;
        char *type;
        KeyValue properties[2];
        bool resume;

        name = cJSON_GetObjectItemCaseSensitive(item, "name");
        assert(name);

        if (cJSON_GetObjectItemCaseSensitive(item, "file"))
            type = "file";
        else
            type = "directory";

        properties[0].key   = "name";
        properties[0].value = name->valuestring;

        properties[1].key   = "type";
        properties[1].value = type;

        resume = callback(properties, sizeof(properties) / sizeof(properties[0]),
                          context);
        if (!resume)
            return;
    }
    callback(NULL, 0, context);
}

static
int onedrive_drive_list_files(HiveDrive *base, const char *path,
                              HiveFilesIterateCallback *callback, void *context)
{
    OneDriveDrive *drive = (OneDriveDrive *)base;
    http_client_t *httpc;
    char url[MAX_URL_LEN] = {0};
    char *next_url = NULL;
    long resp_code;
    cJSON *array;
    cJSON *json = NULL;
    int rc;

    assert(drive);
    assert(drive->token);
    assert(path);
    assert(*path == '/');
    assert(callback);

    rc = oauth_token_check_expire(drive->token);
    if (rc < 0) {
        vlogE("OneDriveDrive: checking access token expired error.");
        return rc;
    }

    if (strlen(path) >= MAX_URL_PARAM_LEN) {
        vlogE("OneDriveDrive: path too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS);
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("OneDriveDrive: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    if (!strcmp(path, "/"))
        sprintf(url, "%s/root/children", MY_DRIVE);
    else
        sprintf(url, "%s/root:%s:/children", MY_DRIVE, path);

    array = cJSON_CreateArray();
    if (!array) {
        vlogE("OneDriveDrive: failed to create json array instance.");
        rc = HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
        goto error_exit;
    }

    next_url = url;
    while (next_url) {
        char *p;
        cJSON *sub_array;
        cJSON *next_link;

        http_client_reset(httpc);
        http_client_set_url(httpc, next_url);
        http_client_set_method(httpc, HTTP_METHOD_GET);
        http_client_set_header(httpc, "Authorization", get_bearer_token(drive->token));
        http_client_enable_response_body(httpc);

        rc = http_client_request(httpc);
        if (json)
            cJSON_Delete(json);

        if (rc) {
            rc = HIVE_CURL_ERROR(rc);
            vlogE("OneDriveDrive: failed to perform http request.");
            break;
        }

        rc = http_client_get_response_code(httpc, &resp_code);
        if (rc) {
            rc = HIVE_CURL_ERROR(rc);
            vlogE("OneDriveDrive: failed to get http response code.");
            break;
        }

        if (resp_code == HttpStatus_Unauthorized) {
            vlogE("OneDriveDrive: access token expired.");
            oauth_token_set_expired(drive->token);
            rc = HIVE_GENERAL_ERROR(HIVEERR_TRY_AGAIN);
            break;
        }

        if (resp_code != HttpStatus_OK) {
            vlogE("OneDriveDrive: error from http response (%d).", resp_code);
            rc = HIVE_HTTP_STATUS_ERROR(resp_code);
            break;
        }

        p = http_client_move_response_body(httpc, NULL);
        if (!p) {
            vlogE("OneDriveDrive: failed to get http response body.");
            rc = HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
            break;
        }

        json = cJSON_Parse(p);
        free(p);
        if (!json) {
            vlogE("OneDriveDrive: bad json format for http response.");
            rc = HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
            break;
        }

        sub_array = cJSON_GetObjectItemCaseSensitive(json, "value");
        if (!sub_array || !cJSON_IsArray(sub_array)) {
            vlogE("OneDriveDrive: missing value json object.");
            cJSON_Delete(json);
            rc = HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
            break;
        }

        rc = merge_array(sub_array, array);
        if (rc < 0) {
            vlogE("OneDriveDrive: failed to merge array.");
            cJSON_Delete(json);
            break;
        }

        next_link = cJSON_GetObjectItemCaseSensitive(json, "@odata.nextLink");
        if (next_link && (!cJSON_IsString(next_link) || !next_link->valuestring ||
                          !*next_link->valuestring)) {
            vlogE("OneDriveDrive: bad format for @odata.nextLink json object.");
            cJSON_Delete(json);
            rc = HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
            break;
        }

        if (next_link)
            next_url = next_link->valuestring;
        else {
            next_url = NULL;
            cJSON_Delete(json);
            notify_user_files(array, callback, context);
            rc = 0;
        }
    }

    cJSON_Delete(array);
    http_client_close(httpc);
    return rc;

error_exit:
    http_client_close(httpc);
    return rc;
}

static
char *create_mkdir_request_body(const char *path)
{
    cJSON *body;
    char *body_str;
    char *p;

    assert(path);

    body = cJSON_CreateObject();
    if (!body) {
        vlogE("OneDriveDrive: failed to create json object.");
        return NULL;
    }

    p = basename((char *)path);
    if (!cJSON_AddStringToObject(body, "name", p) ||
        !cJSON_AddObjectToObject(body, "folder") ||
        !cJSON_AddStringToObject(body, "@microsoft.graph.conflictBehavior", "fail")) {
        vlogE("OneDriveDrive: failed to create json object.");
        cJSON_Delete(body);
        return NULL;
    }

    body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    return body_str;
}

static
int onedrive_drive_mkdir(HiveDrive *base, const char *path)
{
    OneDriveDrive *drive = (OneDriveDrive *)base;
    http_client_t *httpc;
    char url[MAX_URL_LEN] = {0};
    char path_tmp[PATH_MAX];
    char *body;
    char *dir;
    long resp_code = 0;
    int rc;

    assert(drive);
    assert(drive->token);
    assert(path);
    assert(*path);

    rc = oauth_token_check_expire(drive->token);
    if (rc < 0) {
        vlogE("OneDriveDrive: checking access token expired error.");
        return rc;
    }

    if (strlen(path) >= MAX_URL_PARAM_LEN) {
        vlogE("OneDriveDrive: path too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS);
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("OneDriveDrive: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    strcpy(path_tmp, path);
    dir = dirname(path_tmp);
    if (!strcmp(dir, "/"))
        sprintf(url, "%s/root/children", MY_DRIVE);
    else
        sprintf(url, "%s/root:%s:/children", MY_DRIVE, dir);

    body = create_mkdir_request_body(path);
    if (!body) {
        vlogE("OneDriveDrive: failed to create http request body.");
        rc = HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
        goto error_exit;
    }

    http_client_set_url(httpc, url);
    http_client_set_method(httpc, HTTP_METHOD_POST);
    http_client_set_header(httpc, "Content-Type", "application/json");
    http_client_set_header(httpc, "Authorization", get_bearer_token(drive->token));
    http_client_set_request_body_instant(httpc, body, strlen(body));

    rc = http_client_request(httpc);
    free(body);

    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("OneDriveDrive: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    http_client_close(httpc);

    if (rc) {
        vlogE("OneDriveDrive: failed to get http response code.");
        return HIVE_CURL_ERROR(rc);
    }

    if (resp_code == HttpStatus_Unauthorized) {
        vlogE("OneDriveDrive: access token expired.");
        oauth_token_set_expired(drive->token);
        return HIVE_GENERAL_ERROR(HIVEERR_TRY_AGAIN);
    }

    if (resp_code != HttpStatus_Created) {
        vlogE("OneDriveDrive: error from http response (%d).", resp_code);
        return HIVE_HTTP_STATUS_ERROR(resp_code);
    }

    return 0;

error_exit:
    http_client_close(httpc);
    return rc;
}

static char *create_cp_mv_request_body(const char *path)
{
    char url[MAX_URL_LEN] = {0};
    char path_tmp[PATH_MAX];
    cJSON *body;
    cJSON *item;
    cJSON *parent_ref;
    char *body_str;

    body = cJSON_CreateObject();
    if (!body) {
        vlogE("OneDriveDrive: failed to create json object.");
        return NULL;
    }

    parent_ref = cJSON_AddObjectToObject(body, "parentReference");
    if (!parent_ref) {
        vlogE("OneDriveDrive: failed to add parentReference json object.");
        goto error_exit;
    }

    strcpy(path_tmp, path);
    sprintf(url, "/drive/root:%s", dirname(path_tmp));
    item = cJSON_AddStringToObject(parent_ref, "path", url);
    if (!item) {
        vlogE("OneDriveDrive: failed to add path json object.");
        goto error_exit;
    }

    if (!cJSON_AddStringToObject(body, "name", basename((char *)path))) {
        vlogE("OneDriveDrive: failed to add name json object.");
        goto error_exit;
    }

    if (!cJSON_AddStringToObject(body, "@microsoft.graph.conflictBehavior", "replace")) {
        vlogE("OneDriveDrive: failed to add conflictBehavior json object.");
        goto error_exit;
    }

    body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    return body_str;

error_exit:
    cJSON_Delete(body);
    return NULL;
}

static
int onedrive_drive_move_file(HiveDrive *base, const char *old, const char *new)
{
    OneDriveDrive *drive = (OneDriveDrive *)base;
    http_client_t *httpc;
    char url[MAX_URL_LEN] = {0};
    char *body;
    long resp_code = 0;
    int rc;

    assert(drive);
    assert(drive->token);
    assert(old);
    assert(*old);
    assert(new);
    assert(*new);

    rc = oauth_token_check_expire(drive->token);
    if (rc < 0) {
        vlogE("OneDriveDrive: checking access token expired error.");
        return rc;
    }

    if (strlen(old) >= MAX_URL_PARAM_LEN ||
        strlen(new) >= MAX_URL_PARAM_LEN) {
        vlogE("OneDriveDrive: failed to move file: path too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS);
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("OneDriveDrive: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    sprintf(url, "%s/root:%s", MY_DRIVE, old);

    body = create_cp_mv_request_body(new);
    if (!body) {
        vlogE("OneDriveDrive: failed to create request body.");
        rc = HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
        goto error_exit;
    }

    http_client_set_url(httpc, url);
    http_client_set_method(httpc, HTTP_METHOD_PATCH);
    http_client_set_header(httpc, "Content-Type", "application/json");
    http_client_set_header(httpc, "Authorization", get_bearer_token(drive->token));
    http_client_set_request_body_instant(httpc, body, strlen(body));

    rc = http_client_request(httpc);
    free(body);

    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("OneDriveDrive: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    http_client_close(httpc);

    if (rc) {
        vlogE("OneDriveDrive: failed to get http response code.");
        return HIVE_CURL_ERROR(rc);
    }

    if (resp_code == HttpStatus_Unauthorized) {
        vlogE("OneDriveDrive: access token expired.");
        oauth_token_set_expired(drive->token);
        return HIVE_GENERAL_ERROR(HIVEERR_TRY_AGAIN);
    }

    if (resp_code != HttpStatus_OK) {
        vlogE("OneDriveDrive: error from http response (%d).", resp_code);
        return HIVE_HTTP_STATUS_ERROR(resp_code);
    }

    return 0;

error_exit:
    http_client_close(httpc);
    return rc;
}

static
int onedrive_drive_copy_file(HiveDrive *base, const char *src, const char *dest)
{
    OneDriveDrive *drive = (OneDriveDrive *)base;
    http_client_t *httpc;
    char url[MAX_URL_LEN] = {0};
    long resp_code;
    char *body;
    int rc;

    rc = oauth_token_check_expire(drive->token);
    if (rc < 0) {
        vlogE("OneDriveDrive: checking access token expired error.");
        return rc;
    }

    if (strlen(src) >= MAX_URL_PARAM_LEN ||
        strlen(dest) >= MAX_URL_PARAM_LEN) {
        vlogE("OneDriveDrive: path too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS);
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("OneDriveDrive: failed to create http client.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    sprintf(url, "%s/root:%s:/copy", MY_DRIVE, src);
    body = create_cp_mv_request_body(dest);
    if (!body) {
        vlogE("OneDriveDrive: failed to create http request body.");
        rc = HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
        goto error_exit;
    }

    http_client_set_url(httpc, url);
    http_client_set_method(httpc, HTTP_METHOD_POST);
    http_client_set_header(httpc, "Content-Type", "application/json");
    http_client_set_header(httpc, "Authorization", get_bearer_token(drive->token));
    http_client_set_request_body_instant(httpc, body, strlen(body));

    rc = http_client_request(httpc);
    free(body);

    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("OneDriveDrive: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    http_client_close(httpc);

    if (rc) {
        vlogE("OneDriveDrive: failed to get http response code.");
        return HIVE_CURL_ERROR(rc);
    }

    if (resp_code == HttpStatus_Unauthorized) {
        vlogE("OneDriveDrive: access token expired.");
        oauth_token_set_expired(drive->token);
        return HIVE_GENERAL_ERROR(HIVEERR_TRY_AGAIN);
    }

    if (resp_code != HttpStatus_Accepted) {
        vlogE("OneDriveDrive: error from http response (%d).", resp_code);
        return HIVE_HTTP_STATUS_ERROR(resp_code);
    }

    // We will not wait for the completation of copy action.
    return 0;

error_exit:
    http_client_close(httpc);
    return rc;
}

static
int onedrive_drive_delete_file(HiveDrive *base, const char *path)
{
    OneDriveDrive *drive = (OneDriveDrive *)base;
    http_client_t *httpc;
    char url[MAX_URL_LEN] = {0};
    long resp_code = 0;
    int rc;

    assert(drive);
    assert(drive->token);
    assert(path);

    rc = oauth_token_check_expire(drive->token);
    if (rc < 0) {
        vlogE("OneDriveDrive: checking access token expired error.");
        return rc;
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("OneDriveDrive: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    if (strlen(path) >= MAX_URL_PARAM_LEN) {
        vlogE("OneDriveDrive: path too long.");
        rc = HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS);
        goto error_exit;
    }

    sprintf(url, "%s/root:%s:", MY_DRIVE, path);
    http_client_set_url(httpc, url);
    http_client_set_method(httpc, HTTP_METHOD_DELETE);
    http_client_set_header(httpc, "Authorization", get_bearer_token(drive->token));

    rc = http_client_request(httpc);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("OneDriveDrive: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    http_client_close(httpc);

    if (rc) {
        vlogE("OneDriveDrive: failed to get http response code.");
        return HIVE_CURL_ERROR(rc);
    }

    if (resp_code == HttpStatus_Unauthorized) {
        vlogE("OneDriveDrive: access token expired.");
        oauth_token_set_expired(drive->token);
        return HIVE_GENERAL_ERROR(HIVEERR_TRY_AGAIN);
    }

    if (resp_code != HttpStatus_NoContent) {
        vlogE("OneDriveDrive: error from http response (%d).", resp_code);
        return HIVE_HTTP_STATUS_ERROR(resp_code);
    }

    return 0;

error_exit:
    http_client_close(httpc);
    return rc;
}

static int onedrive_drive_open_file(HiveDrive *base, const char *path,
                                    int flags, HiveFile **file)
{
    OneDriveDrive *drive = (OneDriveDrive *)base;

    if (strlen(path) >= MAX_URL_PARAM_LEN) {
        vlogE("OneDriveDrive: path too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS);
    }

    return onedrive_file_open(drive->token, path, flags,
                              drive->tmp_template, file);
}

static void onedrive_drive_close(HiveDrive *base)
{
    assert(base);

    deref(base);
}

static void onedrive_drive_destructor(void *obj)
{
    OneDriveDrive *drive = (OneDriveDrive *)obj;

    if (drive->token)
        oauth_token_delete(drive->token);
}

int onedrive_drive_open(oauth_token_t *token, const char *driveid,
                        const char *tmp_template, HiveDrive **drive)
{
    OneDriveDrive *tmp;

    assert(token);
    assert(driveid);
    assert(drive);

    /*
     * If param @driveid equals "default", then use the default drive.
     * otherwise, use the drive with specific driveid.
     */

    tmp = (OneDriveDrive *)rc_zalloc(sizeof(OneDriveDrive), onedrive_drive_destructor);
    if (!tmp)
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);

    // Add reference of token to drive.
    tmp->token = ref(token);

    tmp->base.get_info    = onedrive_drive_get_info;
    tmp->base.stat_file   = onedrive_drive_stat_file;
    tmp->base.list_files  = onedrive_drive_list_files;
    tmp->base.make_dir    = onedrive_drive_mkdir;
    tmp->base.move_file   = onedrive_drive_move_file;
    tmp->base.copy_file   = onedrive_drive_copy_file;
    tmp->base.delete_file = onedrive_drive_delete_file;
    tmp->base.open_file   = onedrive_drive_open_file;
    tmp->base.close       = onedrive_drive_close;

    sprintf(tmp->tmp_template, "%s", tmp_template);

    *drive = &tmp->base;

    return 0;
}
