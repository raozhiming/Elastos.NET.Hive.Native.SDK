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
#include <assert.h>

#include <crystal.h>
#include <cjson/cJSON.h>

#include "ipfs_utils.h"
#include "ipfs_constants.h"
#include "http_client.h"
#include "hive_error.h"
#include "http_status.h"

static int ipfs_resolve(ipfs_rpc_t *rpc, const char *peerid, char **result)
{
    char url[MAX_URL_LEN] = {0};
    http_client_t *httpc;
    long resp_code = 0;
    char *p;
    int rc;

    rc = snprintf(url, sizeof(url), "http://%s:%u/api/v0/name/resolve",
                  ipfs_rpc_get_current_node_ip(rpc),
                  (unsigned)ipfs_rpc_get_current_node_port(rpc));
    if (rc < 0 || rc >= sizeof(url)) {
        vlogE("IpfsUtils: URL too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL);
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("IpfsUtils: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    http_client_set_url(httpc, url);
    http_client_set_query(httpc, "arg", peerid);
    http_client_set_method(httpc, HTTP_METHOD_GET);
    http_client_enable_response_body(httpc);

    rc = http_client_request(httpc);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("IpfsUtils: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("IpfsUtils: failed to get http response code.");
        goto error_exit;
    }

    if (resp_code != HttpStatus_OK) {
        rc = HIVE_HTTP_STATUS_ERROR(resp_code);
        vlogE("IpfsUtils: error from http response (%d).", resp_code);
        goto error_exit;
    }

    p = http_client_move_response_body(httpc, NULL);
    http_client_close(httpc);

    if (!p) {
        vlogE("IpfsUtils: failed to get http response body.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    *result = p;
    return 0;

error_exit:
    http_client_close(httpc);
    return rc;
}

static int ipfs_login(ipfs_rpc_t *rpc, const char *hash)
{
    char url[MAX_URL_LEN] = {0};
    http_client_t *httpc;
    long resp_code = 0;
    int rc;

    rc = snprintf(url, sizeof(url), "http://%s:%u/api/v0/uid/login",
                  ipfs_rpc_get_current_node_ip(rpc),
                  (unsigned)ipfs_rpc_get_current_node_port(rpc));
    if (rc < 0 || rc >= sizeof(url)) {
        vlogE("IpfsUtils: URL too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL);
    }

    httpc = http_client_new();
    if (!httpc) {
        vlogE("IpfsUtils: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    http_client_set_url(httpc, url);
    http_client_set_query(httpc, "uid", ipfs_rpc_get_uid(rpc));
    http_client_set_query(httpc, "hash", hash);
    http_client_set_method(httpc, HTTP_METHOD_POST);
    http_client_set_request_body_instant(httpc, NULL, 0);

    rc = http_client_request(httpc);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("IpfsUtils: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    http_client_close(httpc);

    if (rc) {
        vlogE("IpfsUtils: failed to get http response code.");
        return HIVE_CURL_ERROR(rc);
    }

    if (resp_code != HttpStatus_OK) {
        vlogE("IpfsUtils: error from http response (%d).", resp_code);
        return HIVE_HTTP_STATUS_ERROR(resp_code);
    }

    return 0;

error_exit:
    http_client_close(httpc);
    return rc;
}

int ipfs_synchronize(ipfs_rpc_t *rpc)
{
    char *resp;
    cJSON *json = NULL;
    cJSON *peer_id;
    cJSON *hash;
    int rc;

    assert(rpc);

    rc = ipfs_rpc_get_uid_info(rpc, &resp);
    if (rc < 0) {
        vlogE("IpfsUtils: get uid info failure.");
        return rc;
    }

    json = cJSON_Parse(resp);
    free(resp);
    if (!json) {
        vlogE("IpfsUtils: bad json format for uid info response.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    peer_id = cJSON_GetObjectItemCaseSensitive(json, "PeerID");
    if (!cJSON_IsString(peer_id) || !peer_id->valuestring || !*peer_id->valuestring) {
        vlogE("IpfsUtils: missing PeerID json object for uid info response.");
        cJSON_Delete(json);
        return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
    }

    rc = ipfs_resolve(rpc, peer_id->valuestring, &resp);
    cJSON_Delete(json);
    if (rc < 0) {
        vlogE("IpfsUtils: failed to resolve uid hash.");
        return rc;
    }

    json = cJSON_Parse(resp);
    free(resp);
    if (!json) {
        vlogE("IpfsUtils: bad json format for resolve response.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    hash = cJSON_GetObjectItemCaseSensitive(json, "Path");
    if (!cJSON_IsString(hash) || !hash->valuestring || !*hash->valuestring) {
        vlogE("IpfsUtils: missing Path json object for resolve response.");
        cJSON_Delete(json);
        return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
    }

    rc = ipfs_login(rpc, hash->valuestring);
    cJSON_Delete(json);
    if (rc < 0) {
        vlogE("IpfsUtils: failed to call login api.");
        return rc;
    }

    return 0;
}

static int get_last_root_hash(ipfs_rpc_t *rpc,
                              char *buf,    // buf used for generating url.
                              size_t bufsz,
                              char *hash,   // to store hash value.
                              size_t length)
{
    http_client_t *httpc;
    long resp_code = 0;
    cJSON *json;
    cJSON *item;
    char *p;
    int rc;

    assert(rpc);
    assert(buf);
    assert(hash);
    assert(bufsz >= MAX_URL_LEN);

    sprintf(buf, "http://%s:%u/api/v0/files/stat",
            ipfs_rpc_get_current_node_ip(rpc),
            (unsigned)ipfs_rpc_get_current_node_port(rpc));

    httpc = http_client_new();
    if (!httpc) {
        vlogE("IpfsUtils: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    http_client_set_url(httpc, buf);
    http_client_set_query(httpc, "uid", ipfs_rpc_get_uid(rpc));
    http_client_set_query(httpc, "path", "/");
    http_client_set_method(httpc, HTTP_METHOD_POST);
    http_client_set_request_body_instant(httpc, NULL, 0);
    http_client_enable_response_body(httpc);

    rc = http_client_request(httpc);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("IpfsUtils: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("IpfsUtils: failed to get http response code.");
        goto error_exit;
    }

    if (resp_code != HttpStatus_OK) {
        vlogE("IpfsUtils: error from http response (%d).", resp_code);
        rc = HIVE_HTTP_STATUS_ERROR(resp_code);
        goto error_exit;
    }

    p = http_client_move_response_body(httpc, NULL);
    http_client_close(httpc);

    if (!p) {
        vlogE("IpfsUtils: failed to get response body.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    json = cJSON_Parse(p);
    free(p);

    if (!json) {
        vlogE("IpfsUtils: bad json format for response body.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    item = cJSON_GetObjectItem(json, "Hash");
    if (!cJSON_IsString(item) ||
        !item->valuestring ||
        !*item->valuestring ||
        strlen(item->valuestring) >= length) {
        cJSON_Delete(json);
        vlogE("IpfsUtils: missing Hash json object for response body.");
        return HIVE_GENERAL_ERROR(HIVEERR_BAD_JSON_FORMAT);
    }

    rc = snprintf(hash, length, "/ipfs/%s", item->valuestring);
    cJSON_Delete(json);

    if (rc < 0 || rc >= length) {
        vlogE("IpfsUtils: hash length too long.");
        return HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL);
    }

    return 0;

error_exit:
    http_client_close(httpc);
    return rc;
}

static int pub_last_root_hash(ipfs_rpc_t *rpc,
                              char *buf,    // buf used for generating url.
                              size_t bufsz, // the length of 'buf'
                              const char *hash)   // the root hash value.
{
    http_client_t *httpc;
    long resp_code = 0;
    int rc;

    assert(rpc);
    assert(buf);
    assert(hash);
    assert(bufsz >= MAX_URL_LEN);

    sprintf(buf, "http://%s:%u/api/v0/name/publish",
            ipfs_rpc_get_current_node_ip(rpc),
            (unsigned)ipfs_rpc_get_current_node_port(rpc));

    httpc = http_client_new();
    if (!httpc) {
        vlogE("IpfsUtils: failed to create http client instance.");
        return HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY);
    }

    http_client_set_url(httpc, buf);
    http_client_set_query(httpc, "uid", ipfs_rpc_get_uid(rpc));
    http_client_set_query(httpc, "path", hash);
    http_client_set_method(httpc, HTTP_METHOD_POST);
    http_client_set_request_body_instant(httpc, NULL, 0);

    rc = http_client_request(httpc);
    if (rc) {
        rc = HIVE_CURL_ERROR(rc);
        vlogE("IpfsUtils: failed to perform http request.");
        goto error_exit;
    }

    rc = http_client_get_response_code(httpc, &resp_code);
    http_client_close(httpc);
    if (rc) {
        vlogE("IpfsUtils: failed to get http response code.");
        return HIVE_CURL_ERROR(rc);
    }

    if (resp_code != HttpStatus_OK) {
        vlogE("IpfsUtils: error from http response (%d).", resp_code);
        return HIVE_HTTP_STATUS_ERROR(resp_code);
    }

    return 0;

error_exit:
    http_client_close(httpc);
    return rc;
}

int publish_root_hash(ipfs_rpc_t *rpc, char *buf, size_t length)
{
    char hash[128] = {0};
    int rc;

    assert(rpc);
    assert(buf);
    assert(length >= MAX_URL_LEN);

    memset(buf, 0, length);
    rc = get_last_root_hash(rpc, buf, length, hash, sizeof(hash));
    if (rc < 0) {
        vlogE("IpfsUtils: get root hash error.");
        return rc;
    }

    memset(buf, 0, length);
    return pub_last_root_hash(rpc, buf, length, hash);
}
