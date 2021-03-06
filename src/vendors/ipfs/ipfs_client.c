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
#include <stddef.h>
#include <pthread.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif
#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <winsock2.h>
#include <winnt.h>
#endif

#include <cjson/cJSON.h>
#include <crystal.h>

#include "ela_hive.h"
#include "ipfs_client.h"
#include "ipfs_drive.h"
#include "ipfs_rpc.h"
#include "ipfs_utils.h"
#include "ipfs_constants.h"
#include "http_client.h"
#include "hive_error.h"
#include "hive_client.h"
#include "mkdirs.h"

typedef struct IPFSClient {
    HiveClient base;
    ipfs_rpc_t *rpc;
    char token_cookie[MAXPATHLEN + 1];
} IPFSClient;

static int ipfs_client_login(HiveClient *base,
                             HiveRequestAuthenticationCallback *cb,
                             void *user_data)
{
    IPFSClient *client = (IPFSClient *)base;
    ipfs_rpc_t *rpc = client->rpc;
    int rc;

    (void)cb;
    (void)user_data;

    rc = ipfs_rpc_check_reachable(client->rpc);
    if (rc < 0) {
        vlogE("IpfsClient: No reachable IPFS node.");
        return rc;
    }

    rc = ipfs_rpc_synchronize(rpc);
    if (RC_NODE_UNREACHABLE(rc)) {
        rc = HIVE_GENERAL_ERROR(HIVEERR_TRY_AGAIN);
        vlogE("IpfsClient: Current IPFS node is unreachable.");
        ipfs_rpc_mark_node_unreachable(client->rpc);
    }

    if (rc < 0) {
        vlogE("IpfsClient: IPFS node synchronization failure.");
        return rc;
    }

    vlogI("IpfsClient: This client logined onto Hive IPFS in success");
    return 0;
}

static int ipfs_client_logout(HiveClient *base)
{
    IPFSClient *client = (IPFSClient *)base;
    ipfs_rpc_t *rpc = client->rpc;
    int rc;

    rc = ipfs_rpc_reset(rpc);
    if (rc < 0) {
        vlogE("IpfsClient: IPFS node synchronization failure.");
        return rc;
    }

    vlogI("IpfsClient: Successfully logout of IPFS.");
    return 0;
}

static int ipfs_client_get_info(HiveClient *base, HiveClientInfo *result)
{
    IPFSClient *client = (IPFSClient *)base;
    ipfs_rpc_t *rpc = client->rpc;
    int rc;

    assert(client);
    assert(result);

    rc = snprintf(result->user_id, sizeof(result->user_id), "%s",
                  ipfs_rpc_get_uid(rpc));
    if (rc < 0 || rc >= sizeof(result->user_id)) {
        vlogE("IpfsClient: Failed to fill user id field of client info.");
        return HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL);
    }

    rc = snprintf(result->display_name, sizeof(result->display_name), "");
    if (rc < 0 || rc >= sizeof(result->display_name)) {
        vlogE("IpfsClient: Failed to fill display name field of client info.");
        return HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL);
    }

    rc = snprintf(result->email, sizeof(result->email), "");
    if (rc < 0 || rc >= sizeof(result->email)) {
        vlogE("IpfsClient: Failed to fill email field of client info.");
        return HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL);
    }

    rc = snprintf(result->phone_number, sizeof(result->phone_number), "");
    if (rc < 0 || rc >= sizeof(result->phone_number)) {
        vlogE("IpfsClient: Failed to fill phone number field of client info.");
        return HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL);
    }

    rc = snprintf(result->region, sizeof(result->region), "");
    if (rc < 0 || rc >= sizeof(result->region)) {
        vlogE("IpfsClient: Failed to fill region field of client info.");
        return HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL);
    }

    return 0;
}

static int ipfs_client_drive_open(HiveClient *base, HiveDrive **drive)
{
    IPFSClient *client = (IPFSClient *)base;
    HiveDrive *tmp;

    assert(base);

    tmp = ipfs_drive_open(client->rpc);
    if (!tmp) {
        vlogE("IpfsClient: Failed to open IPFS drive.");
        return hive_get_error();
    }

    *drive = tmp;

    return 0;
}

static int ipfs_client_close(HiveClient *base)
{
    assert(base);

    deref(base);
    return 0;
}

static void ipfs_client_destructor(void *p)
{
    IPFSClient *client = (IPFSClient *)p;

    if (client->rpc)
        ipfs_rpc_close(client->rpc);
}

static inline bool is_valid_ip(const char *ip)
{
    struct sockaddr_in addr;
    return inet_pton(AF_INET, ip, &addr) > 0 ? true : false;
}

static inline bool is_valid_ipv6(const char *ip)
{
    struct sockaddr_in6 addr;
    return inet_pton(AF_INET6, ip, &addr) > 0 ? true : false;
}

static cJSON *load_ipfs_token_cookie(const char *path)
{
    struct stat st;
    char buf[4096] = {0};
    cJSON *json;
    int rc;
    int fd;

    assert(path);

    rc = stat(path, &st);
    if (rc < 0) {
        vlogE("IpfsClient: Fail to call stat() (%d).", errno);
        return NULL;
    }

    if (!st.st_size || st.st_size > sizeof(buf)) {
        vlogE("IpfsClient: cookie file size does not meet requirement.");
        errno = ERANGE;
        return NULL;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        vlogE("IpfsClient: cookie file can not be opened (%d).", errno);
        return NULL;
    }

    rc = (int)read(fd, buf, st.st_size);
    close(fd);

    if (rc < 0 || rc != st.st_size) {
        vlogE("IpfsClient: read cookie file error.");
        return NULL;
    }

    json = cJSON_Parse(buf);
    if (!json) {
        vlogE("IpfsClient: invalid cookie file content.");
        errno = ENOMEM;
        return NULL;
    }

    vlogI("IpfsClient: Successfully loaded rpc from cookie.");

    return json;
}

static int writeback_token(const cJSON *json, void *user_data)
{
    IPFSClient *client = (IPFSClient *)user_data;
    char *json_str;
    int json_str_len;
    int fd;
    int bytes;

    json_str = cJSON_PrintUnformatted(json);
    if (!json_str || !*json_str) {
        vlogE("IpfsClient: invalid rpc format.");
        errno = ENOMEM;
        return -1;
    }

    fd = open(client->token_cookie, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        vlogE("IpfsClient: failed to open cookie file (%d).", errno);
        free(json_str);
        return -1;
    }

    json_str_len = (int)strlen(json_str);
    bytes = (int)write(fd, json_str, json_str_len + 1);
    free(json_str);
    close(fd);

    if (bytes != json_str_len + 1) {
        vlogE("IpfsClient: failed to write to cookie file.");
        return -1;
    }

    vlogI("IpfsClient: Successfully save rpc to cookie file.");

    return 0;
}

HiveClient *ipfs_client_new(const HiveOptions *options)
{
    IPFSOptions *opts = (IPFSOptions *)options;
    ipfs_rpc_options_t *token_options;
    size_t token_options_sz;
    char token_cookie[MAXPATHLEN + 1];
    cJSON *token_cookie_json = NULL;
    char path_tmp[PATH_MAX];
    IPFSClient *client;
    int rc;
    size_t i;

    assert(options);

    token_options_sz = sizeof(*token_options) +
                       sizeof(rpc_node_t) * opts->rpc_node_count;
    token_options = alloca(token_options_sz);
    memset(token_options, 0, token_options_sz);

    // check uid configuration
    if (opts->uid && (!*opts->uid || strlen(opts->uid) >= sizeof(token_options->uid)))
        return NULL;

    if (opts->uid)
        strcpy(token_options->uid, opts->uid);

    // check bootstraps configuration
    if (!opts->rpc_node_count)
        return NULL;

    token_options->rpc_nodes_count = opts->rpc_node_count;
    for (i = 0; i < opts->rpc_node_count; ++i) {
        HiveRpcNode *node = &opts->rpcNodes[i];
        rpc_node_t *node_options = &token_options->rpc_nodes[i];
        size_t ipv4_len;
        size_t ipv6_len;
        char *endptr;
        long port;

        ipv4_len = node->ipv4 ? strlen(node->ipv4) : 0;
        ipv6_len = node->ipv6 ? strlen(node->ipv6) : 0;

        if (!ipv4_len && !ipv6_len)
            return NULL;

        if (ipv4_len) {
            if (ipv4_len >= sizeof(node_options->ipv4))
                return NULL;

            if (!is_valid_ip(node->ipv4))
                return NULL;

            strcpy(node_options->ipv4, node->ipv4);
        }

        if (ipv6_len) {
            if (ipv6_len >= sizeof(node_options->ipv6))
                return NULL;

            if (!is_valid_ipv6(node->ipv6))
                return NULL;

            strcpy(node_options->ipv6, node->ipv6);
        }

        if (!node->port || !*node->port)
            return NULL;

        port = strtol(node->port, &endptr, 10);
        if (*endptr)
            return NULL;

        if (port < 1 || port > 65535)
            return NULL;

        node_options->port = (uint16_t)port;
    }

    // check rpc cookie
    rc = snprintf(token_cookie, sizeof(token_cookie), "%s/.data/ipfs.json",
                  opts->base.persistent_location);
    if (rc < 0 || rc >= sizeof(token_cookie)) {
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_BUFFER_TOO_SMALL));
        return NULL;
    }

    strcpy(path_tmp, token_cookie);
    rc = mkdirs(dirname(path_tmp), S_IRWXU);
    if (rc < 0 && errno != EEXIST) {
        vlogE("IpfsClient: failed to create directory (%d).", errno);
        hive_set_error(HIVE_SYS_ERROR(errno));
        return NULL;
    }

    if (!access(token_cookie, F_OK)) {
        token_cookie_json = load_ipfs_token_cookie(token_cookie);
        if (!token_cookie_json) {
            vlogE("IpfsClient: load rpc from cookie error.");
            hive_set_error(HIVE_SYS_ERROR(errno));
            return NULL;
        }
    }

    token_options->store = token_cookie_json;

    client = (IPFSClient *)rc_zalloc(sizeof(IPFSClient), &ipfs_client_destructor);
    if (!client) {
        if (token_options->store)
            cJSON_Delete(token_options->store);
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_OUT_OF_MEMORY));
        return NULL;
    }

    strcpy(client->token_cookie, token_cookie);

    client->base.login       = &ipfs_client_login;
    client->base.logout      = &ipfs_client_logout;
    client->base.get_info    = &ipfs_client_get_info;
    client->base.get_drive   = &ipfs_client_drive_open;
    client->base.close       = &ipfs_client_close;

    client->rpc = ipfs_rpc_new(token_options, &writeback_token, client);
    if (token_options->store)
        cJSON_Delete(token_options->store);
    if (!client->rpc) {
        vlogE("IpfsClient: failed to create IPFS rpc.");
        deref(client);
        return NULL;
    }

    return &client->base;
}
