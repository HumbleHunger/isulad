/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: wangfengtu
 * Create: 2020-03-05
 * Description: provide registry api v2 functions
 ******************************************************************************/

#define _GNU_SOURCE /* See feature_test_macros(7) */
#include "registry_apiv2.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <http_parser.h>
#include <isula_libutils/json_common.h>
#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>

#include "registry_type.h"
#include "isula_libutils/log.h"
#include "http.h"
#include "http_request.h"
#include "utils.h"
#include "parser.h"
#include "mediatype.h"
#include "isula_libutils/oci_image_index.h"
#include "isula_libutils/registry_manifest_list.h"
#include "auths.h"
#include "err_msg.h"
#include "sha256.h"
#include "utils_array.h"
#include "utils_file.h"
#include "utils_string.h"
#include "utils_verify.h"

#define DOCKER_API_VERSION_HEADER "Docker-Distribution-Api-Version: registry/2.0"
#define MAX_ACCEPT_LEN 128
// retry 5 times
#define RETRY_TIMES 5
#define BODY_DELIMITER "\r\n\r\n"

static void set_body_null_if_exist(char *message)
{
    char *body = NULL;

    body = strstr(message, BODY_DELIMITER);
    if (body != NULL) {
        *(body + strlen(BODY_DELIMITER)) = 0;
    }
}

static int parse_http_header(char *resp_buf, size_t buf_size, struct parsed_http_message *message)
{
    char *real_message = NULL;
    int ret = 0;

    if (resp_buf == NULL || message == NULL) {
        ERROR("Invalid NULL param");
        return -1;
    }

    real_message = strstr(resp_buf, "HTTP/1.1");
    if (real_message == NULL) {
        ERROR("Failed to parse response, the response do not have HTTP/1.1");
        ret = -1;
        goto out;
    }

    set_body_null_if_exist(real_message);

    ret = parse_http(real_message, strlen(real_message), message, HTTP_RESPONSE);
    if (ret != 0) {
        ERROR("Failed to parse response: %s", real_message);
        ret = -1;
        goto out;
    }

out:

    return ret;
}

static int parse_challenges(pull_descriptor *desc, char *schema, char *params)
{
    challenge c = { 0 };
    char **kv_strs = NULL;
    char **kv = NULL;
    size_t len = 0;
    size_t i = 0;
    char *value = NULL;
    int ret = -1;

    // Support "Bearer" and "Basic" only.
    if (!strcasecmp(schema, "Bearer")) {
        // params:
        // realm="https://auth.isula.org/token",service="registry.isula.org"
        params = util_trim_space(params);
        kv_strs = util_string_split(params, ',');
        len = util_array_len((const char **)kv_strs);
        for (i = 0; i < len; i++) {
            kv = util_string_split(kv_strs[i], '=');
            if (util_array_len((const char **)kv) != 2) {
                ERROR("Split key/value failed, origin string is %s", kv_strs[i]);
                ret = -1;
                goto out;
            }
            value = util_trim_space(kv[1]);
            value = util_trim_quotation(value);
            if (!strcmp(kv[0], "realm")) {
                free(c.realm);
                c.realm = util_strdup_s(value);
            } else if (!strcmp(kv[0], "service")) {
                free(c.service);
                c.service = util_strdup_s(value);
            }
            util_free_array(kv);
            kv = NULL;
        }
    } else if (!strcasecmp(schema, "Basic")) {
        c.realm = util_strdup_s(schema);
    } else {
        WARN("Found unsupported schema %s", schema);
        ret = -1;
        goto out;
    }
    c.schema = util_strdup_s(schema);

    for (i = 0; i < CHALLENGE_MAX; i++) {
        // schema == NULL means this challenge have not be used.
        if (desc->challenges[i].schema == NULL) {
            desc->challenges[i] = c;
            ret = 0;
            goto out;
        }
    }

    WARN("Too many challenges found, keep %d only", CHALLENGE_MAX);
    ret = -1;

out:
    if (ret != 0) {
        free_challenge(&c);
    }
    util_free_array(kv_strs);
    kv_strs = NULL;
    util_free_array(kv);
    kv = NULL;

    return ret;
}

static int parse_auth(pull_descriptor *desc, char *auth)
{
    char *origin_tmp_auth = NULL;
    char *trimmed_auth = NULL;
    int ret = 0;
    char *schema = NULL;
    char *params = NULL;

    if (auth == NULL) {
        return -1;
    }

    // auth: Bearer realm="https://auth.isula.org/token",service="isula registry"
    origin_tmp_auth = util_strdup_s(auth);
    util_trim_newline(origin_tmp_auth);
    trimmed_auth = util_trim_space(origin_tmp_auth);
    params = strchr(trimmed_auth, ' ');
    if (params == NULL) {
        ERROR("invalid auth when parse challenges, auth: %s", trimmed_auth);
        ret = -1;
        goto out;
    }
    // params: realm="https://auth.isula.org/token",service="isula registry"
    params[0] = 0;
    params += 1;
    // schema: Bearer
    schema = trimmed_auth;

    ret = parse_challenges(desc, schema, params);
    if (ret != 0) {
        ERROR("Parse challenges failed, schema: %s, params: %s", schema, params);
        ret = -1;
        goto out;
    }

out:
    free(origin_tmp_auth);
    origin_tmp_auth = NULL;

    return ret;
}

static int parse_auths(pull_descriptor *desc, struct parsed_http_message *m)
{
    int i = 0;
    int ret = 0;

    for (i = 0; i < m->num_headers; i++) {
        if (!strcasecmp(m->headers[i][0], "Www-Authenticate")) {
            ret = parse_auth(desc, (char *)m->headers[i][1]);
            if (ret != 0) {
                WARN("parse auth %s failed", (char *)m->headers[i][1]);
            }
        }
    }

    return ret;
}

static void free_parsed_http_message(struct parsed_http_message **message)
{
    if (message == NULL || *message == NULL) {
        return;
    }
    free((*message)->body);
    (*message)->body = NULL;
    free(*message);
    *message = NULL;
    return;
}

static struct parsed_http_message *get_parsed_message(char *http_head)
{
    int ret = 0;
    struct parsed_http_message *message = NULL;

    message = (struct parsed_http_message *)util_common_calloc_s(sizeof(struct parsed_http_message));
    if (message == NULL) {
        ERROR("out of memory");
        ret = -1;
        goto out;
    }

    ret = parse_http_header(http_head, strlen(http_head), message);
    if (ret != 0) {
        ERROR("parse http header failed");
        ret = -1;
        goto out;
    }

out:
    if (ret != 0) {
        free_parsed_http_message(&message);
    }

    return message;
}

static int parse_ping_header(pull_descriptor *desc, char *http_head)
{
    struct parsed_http_message *message = NULL;
    char *version = NULL;
    int ret = 0;

    if (http_head == NULL) {
        ERROR("Invalid NULL param");
        return -1;
    }

    /*
        Response data:
        HTTP/1.0 200 Connection established
        HTTP/1.1 401 Unauthorized
        Content-Type: application/json
        Docker-Distribution-Api-Version: registry/2.0
        Www-Authenticate: Bearer realm="https://auth.isula.org/token",service="isula registry"
        Date: Mon, 16 Mar 2020 01:16:09 GMT
        Content-Length: 87
        Strict-Transport-Security: max-age=31536000

        {"errors":[{"code":"UNAUTHORIZED","message":"authentication required","detail":null}]}
    */
    message = get_parsed_message(http_head);
    if (message == NULL) {
        ERROR("Get parsed message failed. http response size %zu, response:%s", strlen(http_head), http_head);
        ret = -1;
        goto out;
    }

    if (message->status_code != StatusUnauthorized && message->status_code != StatusOK) {
        ERROR("registry response invalid status code %d", message->status_code);
        ret = -1;
        goto out;
    }

    version = get_header_value(message, "Docker-Distribution-Api-Version");
    if (version == NULL) {
        ERROR("Docker-Distribution-Api-Version not found in header, registry may can not support registry API V2");
        ret = -1;
        goto out;
    }

    if (!util_strings_contains_word(version, "registry/2.0")) {
        ERROR("Docker-Distribution-Api-Version does not contain registry/2.0, it's value is %s."
              "Registry can not support registry API V2",
              version);
        ret = -1;
        goto out;
    }

    ret = parse_auths(desc, message);
    if (ret != 0) {
        ERROR("Parse WWW-Authenticate header failed");
        goto out;
    }

out:
    if (ret != 0) {
        ERROR("ping resp=%s", http_head);
    }

    free_parsed_http_message(&message);

    return ret;
}

int registry_pingv2(pull_descriptor *desc, char *protocol)
{
    int ret = 0;
    int sret = 0;
    char *output = NULL;
    char url[PATH_MAX] = { 0 };
    char **headers = NULL;

    if (desc == NULL) {
        ERROR("Invalid NULL param");
        return -1;
    }

    sret = snprintf(url, sizeof(url), "%s://%s/v2/", protocol, desc->host);
    if (sret < 0 || (size_t)sret >= sizeof(url)) {
        ERROR("Failed to sprintf url for ping, host is %s", desc->host);
        ret = -1;
        goto out;
    }

    ret = util_array_append(&headers, DOCKER_API_VERSION_HEADER);
    if (ret != 0) {
        ERROR("Append api version to header failed");
        ret = -1;
        goto out;
    }

    // Sending url
    // https://registry.isula.org/v2/
    INFO("sending ping url: %s", url);
    ret = http_request_buf(desc, url, (const char **)headers, &output, HEAD_BODY);
    if (ret != 0) {
        ERROR("http request failed");
        goto out;
    }
    DEBUG("ping resp=%s", output);

    ret = parse_ping_header(desc, output);
    if (ret != 0) {
        ERROR("parse ping header failed, response: %s", output);
        goto out;
    }

out:
    free(output);
    output = NULL;
    util_free_array(headers);
    headers = NULL;

    return ret;
}

static int registry_ping(pull_descriptor *desc)
{
    int ret = 0;

    if (desc == NULL) {
        ERROR("Invalid NULL param");
        return -1;
    }

    if (desc->protocol != NULL) {
        return 0;
    }

    ret = registry_pingv2(desc, "https");
    if (ret == 0) {
        desc->protocol = util_strdup_s("https");
        goto out;
    }

    if (desc->insecure_registry) {
        ERROR("ping %s with https failed, try http", desc->host);

        DAEMON_CLEAR_ERRMSG();
        ret = registry_pingv2(desc, "http");
        if (ret != 0) {
            ERROR("ping %s with http failed", desc->host);
            goto out;
        }
        desc->protocol = util_strdup_s("http");
    } else {
        ERROR("ping %s with https failed", desc->host);
    }

out:

    return ret;
}

static int registry_request(pull_descriptor *desc, char *path, char **custom_headers, char *file, char **output_buffer,
                            resp_data_type type, CURLcode *errcode)
{
    int ret = 0;
    int sret = 0;
    char url[PATH_MAX] = { 0 };
    char **headers = NULL;

    if (desc == NULL || path == NULL || (file == NULL && output_buffer == NULL)) {
        ERROR("Invalid NULL param");
        return -1;
    }

    ret = registry_ping(desc);
    if (ret != 0) {
        ERROR("ping failed");
        return -1;
    }

    sret = snprintf(url, sizeof(url), "%s://%s%s", desc->protocol, desc->host, path);
    if (sret < 0 || (size_t)sret >= sizeof(url)) {
        ERROR("Failed to sprintf url, path is %s", path);
        ret = -1;
        goto out;
    }

    headers = util_str_array_dup((const char **)custom_headers, util_array_len((const char **)custom_headers));
    if (ret != 0) {
        ERROR("duplicate custom headers failed");
        ret = -1;
        goto out;
    }

    ret = util_array_append(&headers, DOCKER_API_VERSION_HEADER);
    if (ret != 0) {
        ERROR("Append api version to header failed");
        ret = -1;
        goto out;
    }

    DEBUG("sending url: %s", url);
    if (output_buffer != NULL) {
        ret = http_request_buf(desc, url, (const char **)headers, output_buffer, type);
        if (ret != 0) {
            ERROR("http request buffer failed, url: %s", url);
            goto out;
        }
        DEBUG("resp=%s", *output_buffer);
    } else {
        ret = http_request_file(desc, url, (const char **)headers, file, type, errcode);
        if (ret != 0) {
            ERROR("http request file failed, url: %s", url);
            goto out;
        }
    }

out:
    util_free_array(headers);
    headers = NULL;

    return ret;
}

static int check_content_type(const char *content_type)
{
    if (content_type == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    if (!strcmp(content_type, DOCKER_MANIFEST_SCHEMA1_JSON) ||
        !strcmp(content_type, DOCKER_MANIFEST_SCHEMA1_PRETTYJWS) ||
        !strcmp(content_type, DOCKER_MANIFEST_SCHEMA2_JSON) || !strcmp(content_type, DOCKER_MANIFEST_SCHEMA2_LIST) ||
        !strcmp(content_type, OCI_MANIFEST_V1_JSON) || !strcmp(content_type, MEDIA_TYPE_APPLICATION_JSON) ||
        !strcmp(content_type, OCI_INDEX_V1_JSON)) {
        return 0;
    }

    return -1;
}

static int parse_manifest_head(char *http_head, char **content_type, char **digest)
{
    int ret = 0;
    struct parsed_http_message *message = NULL;
    char *value = NULL;

    if (http_head == NULL || content_type == NULL || digest == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    // HTTP/1.0 200 Connection established
    //
    // HTTP/1.1 200 OK
    // Content-Length: 948
    // Content-Type: application/vnd.docker.distribution.manifest.v2+json
    // Docker-Content-Digest: sha256:788fa27763db6d69ad3444e8ba72f947df9e7e163bad7c1f5614f8fd27a311c3
    // Docker-Distribution-Api-Version: registry/2.0
    // Etag: "sha256:788fa27763db6d69ad3444e8ba72f947df9e7e163bad7c1f5614f8fd27a311c3"
    // Date: Thu, 27 Jul 2017 09:14:10 GMT
    // Strict-Transport-Security: max-age=31536000
    message = get_parsed_message(http_head);
    if (message == NULL) {
        ERROR("parse http header message for manifests failed, message: %s", http_head);
        ret = -1;
        goto out;
    }

    if (message->status_code != StatusOK) {
        ERROR("registry response invalid status code %d\nresponse:%s", message->status_code, http_head);
        if (message->status_code == StatusNotFound) {
            isulad_try_set_error_message("Image not found in registry");
        } else {
            isulad_try_set_error_message("registry response invalid status code %d", message->status_code);
        }
        ret = -1;
        goto out;
    }

    value = get_header_value(message, "Content-Type");
    if (value == NULL) {
        ERROR("Get content type from message header failed, response: %s", http_head);
        ret = -1;
        goto out;
    }

    ret = check_content_type(value);
    if (ret != 0) {
        ERROR("Unsupported content type: %s", value);
        goto out;
    }
    *content_type = util_strdup_s(value);

    value = get_header_value(message, "Docker-Content-Digest");
    if (value != NULL) { // No Docker-Content-Digest is also valid
        if (!util_valid_digest(value)) {
            ERROR("Invalid content digest: %s", value);
            goto out;
        }
        *digest = util_strdup_s(value);
    }

out:

    if (ret != 0) {
        free(*content_type);
        *content_type = NULL;
        free(*digest);
        *digest = NULL;
    }

    free_parsed_http_message(&message);

    return ret;
}

static int append_manifests_accepts(char ***custom_headers)
{
    int i = 0;
    int ret = 0;
    int sret = 0;
    char accept[MAX_ACCEPT_LEN] = { 0 };
    const char *mediatypes[] = { DOCKER_MANIFEST_SCHEMA2_JSON,
                                 DOCKER_MANIFEST_SCHEMA1_PRETTYJWS,
                                 DOCKER_MANIFEST_SCHEMA1_JSON,
                                 DOCKER_MANIFEST_SCHEMA2_LIST,
                                 MEDIA_TYPE_APPLICATION_JSON,
                                 OCI_MANIFEST_V1_JSON,
                                 OCI_INDEX_V1_JSON
                               };

    for (i = 0; i < sizeof(mediatypes) / sizeof(mediatypes[0]); i++) {
        sret = snprintf(accept, MAX_ACCEPT_LEN, "Accept: %s", mediatypes[i]);
        if (sret < 0 || (size_t)sret >= MAX_ACCEPT_LEN) {
            ERROR("Failed to sprintf accept media type %s", mediatypes[i]);
            ret = -1;
            goto out;
        }

        ret = util_array_append(custom_headers, accept);
        if (ret != 0) {
            ERROR("append accepts failed");
            goto out;
        }
    }

out:

    return ret;
}

static int split_head_body(char *file, char **http_head)
{
    int ret = 0;
    char *all = NULL;
    char *head = NULL;
    char *deli = "\r\n\r\n"; // default delimiter, may change in later process
    char *body = NULL;

    all = util_read_text_file(file);
    if (all == NULL) {
        ERROR("read file %s failed", file);
        return -1;
    }

    head = strstr(all, "HTTP/1.1");
    if (head == NULL) {
        ERROR("No HTTP/1.1 found");
        ret = -1;
        goto out;
    }
    body = strstr(head, deli);
    if (body == NULL) {
        deli = "\n\n";
        body = strstr(head, deli);
        if (body == NULL) {
            ERROR("No body found, data=%s", head);
            ret = -1;
            goto out;
        }
    }
    body += strlen(deli);

    ret = util_write_file(file, body, strlen(body), 0600);
    if (ret != 0) {
        ERROR("rewrite body to file failed");
        ret = -1;
        goto out;
    }
    *body = 0;

    *http_head = util_strdup_s(head);

out:
    free(all);
    all = NULL;

    return ret;
}

static int fetch_manifest_list(pull_descriptor *desc, char *file, char **content_type, char **digest)
{
    int ret = 0;
    int sret = 0;
    char *http_head = NULL;
    char **custom_headers = NULL;
    char path[PATH_MAX] = { 0 };
    CURLcode errcode = CURLE_OK;
    int retry_times = RETRY_TIMES;

    if (desc == NULL || content_type == NULL || digest == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    ret = append_manifests_accepts(&custom_headers);
    if (ret != 0) {
        ERROR("append accepts failed");
        goto out;
    }

    sret = snprintf(path, sizeof(path), "/v2/%s/manifests/%s", desc->name, desc->tag);
    if (sret < 0 || (size_t)sret >= sizeof(path)) {
        ERROR("Failed to sprintf path for manifest");
        ret = -1;
        goto out;
    }

    while (retry_times > 0) {
        retry_times--;
        ret = registry_request(desc, path, custom_headers, file, NULL, HEAD_BODY, &errcode);
        if (ret != 0) {
            if (retry_times > 0 && !desc->cancel) {
                continue;
            }
            ERROR("registry: Get %s failed", path);
            goto out;
        }
        break;
    }

    ret = split_head_body(file, &http_head);
    if (ret != 0) {
        ERROR("registry: Split %s to head body failed", file);
        goto out;
    }

    ret = parse_manifest_head(http_head, content_type, digest);
    if (ret != 0) {
        ret = -1;
        goto out;
    }

out:

    free(http_head);
    http_head = NULL;
    util_free_array(custom_headers);
    custom_headers = NULL;

    return ret;
}

static void try_log_resp_body(char *path, char *file)
{
    char *body = NULL;
    size_t size = 0;

    body = util_read_text_file(file);
    if (body == NULL) {
        return;
    }

    size = strlen(body);
    if (size < LXC_LOG_BUFFER_SIZE) { // Max length not exactly, just avoid too long.
        ERROR("Get %s response message body: %s", path, body);
    }
    free(body);
    return;
}

static int fetch_data(pull_descriptor *desc, char *path, char *file, char *content_type, char *digest)
{
    int ret = 0;
    int sret = 0;
    char accept[MAX_ELEMENT_SIZE] = { 0 };
    char **custom_headers = NULL;
    int retry_times = RETRY_TIMES;
    resp_data_type type = BODY_ONLY;
    bool forbid_resume = false;
    CURLcode errcode = CURLE_OK;

    // digest can be NULL
    if (desc == NULL || path == NULL || file == NULL || content_type == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    sret = snprintf(accept, MAX_ACCEPT_LEN, "Accept: %s", content_type);
    if (sret < 0 || (size_t)sret >= MAX_ACCEPT_LEN) {
        ERROR("Failed to sprintf accept media type %s", content_type);
        ret = -1;
        goto out;
    }

    ret = util_array_append(&custom_headers, accept);
    if (ret != 0) {
        ERROR("append accepts failed");
        goto out;
    }

    while (retry_times > 0) {
        retry_times--;
        ret = registry_request(desc, path, custom_headers, file, NULL, type, &errcode);
        if (ret != 0) {
            if (errcode == CURLE_RANGE_ERROR) {
                forbid_resume = true;
                type = BODY_ONLY;
            }
            if (!forbid_resume) {
                type = RESUME_BODY;
            }
            if (retry_times > 0 && !desc->cancel) {
                continue;
            }
            ERROR("registry: Get %s failed", path);
            isulad_try_set_error_message("Get %s failed", path);
            desc->cancel = true;
            goto out;
        }

        // If content is signatured, digest is for payload but not fetched data
        if (strcmp(content_type, DOCKER_MANIFEST_SCHEMA1_PRETTYJWS) && digest != NULL) {
            if (!sha256_valid_digest_file(file, digest)) {
                type = BODY_ONLY;
                if (retry_times > 0 && !desc->cancel) {
                    continue;
                }
                ret = -1;
                try_log_resp_body(path, file);
                ERROR("data from %s does not have digest %s", path, digest);
                isulad_try_set_error_message("Invalid data fetched for %s, this mainly caused by server error", path);
                desc->cancel = true;
                goto out;
            }
        }
        break;
    }

out:
    util_free_array(custom_headers);
    custom_headers = NULL;

    return ret;
}

static bool is_variant_same(char *variant1, char *variant2)
{
    // Compatible with manifests which didn't have variant
    if (variant1 == NULL || variant2 == NULL) {
        return true;
    }
    return !strcasecmp(variant1, variant2);
}

static int select_oci_manifest(oci_image_index *index, char **content_type, char **digest)
{
    size_t i = 0;
    int ret = 0;
    char *host_os = NULL;
    char *host_arch = NULL;
    char *host_variant = NULL;
    oci_image_index_manifests_platform *platform = NULL;
    bool found = false;

    if (index == NULL || content_type == NULL || digest == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    ret = util_normalized_host_os_arch(&host_os, &host_arch, &host_variant);
    if (ret != 0) {
        ret = -1;
        goto out;
    }

    for (i = 0; i < index->manifests_len; i++) {
        platform = index->manifests[i]->platform;
        if (platform == NULL || platform->architecture == NULL || platform->os == NULL) {
            continue;
        }
        if (!strcasecmp(platform->architecture, host_arch) && !strcasecmp(platform->os, host_os) &&
            is_variant_same(host_variant, platform->variant)) {
            free(*content_type);
            *content_type = util_strdup_s(index->manifests[i]->media_type);
            free(*digest);
            *digest = util_strdup_s(index->manifests[i]->digest);
            found = true;
            goto out;
        }
    }

    ret = -1;
    ERROR("Cann't match any manifest, host os %s, host arch %s, host variant %s", host_os, host_arch, host_variant);

out:
    free(host_os);
    host_os = NULL;
    free(host_arch);
    host_arch = NULL;
    free(host_variant);
    host_variant = NULL;

    if (found && (*digest == NULL || *content_type == NULL)) {
        ERROR("Matched manifest have NULL digest or mediatype in manifest, mediatype %s, digest %s", *content_type,
              *digest);
        ret = -1;
    }

    return ret;
}

static int select_docker_manifest(registry_manifest_list *manifests, char **content_type, char **digest)
{
    size_t i = 0;
    int ret = 0;
    char *host_os = NULL;
    char *host_arch = NULL;
    char *host_variant = NULL;
    registry_manifest_list_manifests_platform *platform = NULL;
    bool found = false;

    if (manifests == NULL || content_type == NULL || digest == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    ret = util_normalized_host_os_arch(&host_os, &host_arch, &host_variant);
    if (ret != 0) {
        ret = -1;
        goto out;
    }

    for (i = 0; i < manifests->manifests_len; i++) {
        platform = manifests->manifests[i]->platform;
        if (platform == NULL || platform->architecture == NULL || platform->os == NULL) {
            continue;
        }
        if (!strcasecmp(platform->architecture, host_arch) && !strcasecmp(platform->os, host_os) &&
            is_variant_same(host_variant, platform->variant)) {
            free(*content_type);
            *content_type = util_strdup_s(manifests->manifests[i]->media_type);
            free(*digest);
            *digest = util_strdup_s(manifests->manifests[i]->digest);
            found = true;
            goto out;
        }
    }

    ret = -1;
    ERROR("Cann't match any manifest, host os %s, host arch %s, host variant %s", host_os, host_arch, host_variant);

out:
    free(host_os);
    host_os = NULL;
    free(host_arch);
    host_arch = NULL;
    free(host_variant);
    host_variant = NULL;

    if (found && (*digest == NULL || *content_type == NULL)) {
        ERROR("Matched manifest have NULL digest or mediatype in manifest, mediatype %s, digest %s", *content_type,
              *digest);
        ret = -1;
    }

    return ret;
}

static int select_manifest(char *file, char **content_type, char **digest)
{
    int ret = 0;
    oci_image_index *index = NULL;
    registry_manifest_list *manifests = NULL;
    parser_error err = NULL;

    if (file == NULL || content_type == NULL || *content_type == NULL || digest == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    if (!strcmp(*content_type, OCI_INDEX_V1_JSON)) {
        index = oci_image_index_parse_file((const char *)file, NULL, &err);
        if (index == NULL) {
            ERROR("parse oci image index failed: %s", err);
            ret = -1;
            goto out;
        }
        ret = select_oci_manifest(index, content_type, digest);
        if (ret != 0) {
            ERROR("select oci manifest failed");
            ret = -1;
            goto out;
        }
    } else if (!strcmp(*content_type, DOCKER_MANIFEST_SCHEMA2_LIST)) {
        manifests = registry_manifest_list_parse_file((const char *)file, NULL, &err);
        if (manifests == NULL) {
            ERROR("parse docker image manifest list failed: %s", err);
            ret = -1;
            goto out;
        }

        ret = select_docker_manifest(manifests, content_type, digest);
        if (ret != 0) {
            ERROR("select docker manifest failed");
            ret = -1;
            goto out;
        }
    } else {
        // This should not happen
        ERROR("Unexpected content type %s", *content_type);
        ret = -1;
        goto out;
    }

out:
    if (index != NULL) {
        free_oci_image_index(index);
        index = NULL;
    }
    if (manifests != NULL) {
        free_registry_manifest_list(manifests);
        manifests = NULL;
    }
    free(err);
    err = NULL;

    return ret;
}

static int fetch_manifest_data(pull_descriptor *desc, char *file, char **content_type, char **digest)
{
    int ret = 0;
    int sret = 0;
    char path[PATH_MAX] = { 0 };
    char *manifest_text = NULL;

    if (desc == NULL || file == NULL || content_type == NULL || *content_type == NULL || digest == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    // If it's manifest list, we must choose the manifest which match machine's architecture to download.
    if (!strcmp(*content_type, DOCKER_MANIFEST_SCHEMA2_LIST) || !strcmp(*content_type, OCI_INDEX_V1_JSON)) {
        ret = select_manifest(file, content_type, digest);
        if (ret != 0) {
            manifest_text = util_read_text_file(file);
            ERROR("select manifest failed, manifests:%s", manifest_text);
            free(manifest_text);
            manifest_text = NULL;
            goto out;
        }

        sret = snprintf(path, sizeof(path), "/v2/%s/manifests/%s", desc->name, *digest);
        if (sret < 0 || (size_t)sret >= sizeof(path)) {
            ERROR("Failed to sprintf path for manifest");
            ret = -1;
            goto out;
        }

        ret = fetch_data(desc, path, file, *content_type, *digest);
        if (ret != 0) {
            ERROR("registry: Get %s failed", path);
            goto out;
        }
    }

out:

    return ret;
}

int fetch_manifest(pull_descriptor *desc)
{
    int ret = 0;
    int sret = 0;
    char file[PATH_MAX] = { 0 };
    char *content_type = NULL;
    char *digest = NULL;

    if (desc == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    sret = snprintf(file, sizeof(file), "%s/manifests", desc->blobpath);
    if (sret < 0 || (size_t)sret >= sizeof(file)) {
        ERROR("Failed to sprintf file for manifest");
        return -1;
    }

    ret = fetch_manifest_list(desc, file, &content_type, &digest);
    if (ret != 0) {
        ret = -1;
        goto out;
    }

    ret = fetch_manifest_data(desc, file, &content_type, &digest);
    if (ret != 0) {
        ret = -1;
        goto out;
    }

    desc->manifest.media_type = util_strdup_s(content_type);
    desc->manifest.digest = util_strdup_s(digest);
    desc->manifest.file = util_strdup_s(file);

out:
    free(content_type);
    content_type = NULL;
    free(digest);
    digest = NULL;

    return ret;
}

int fetch_config(pull_descriptor *desc)
{
    int ret = 0;
    int sret = 0;
    char file[PATH_MAX] = { 0 };
    char path[PATH_MAX] = { 0 };

    if (desc == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    sret = snprintf(file, sizeof(file), "%s/config", desc->blobpath);
    if (sret < 0 || (size_t)sret >= sizeof(file)) {
        ERROR("Failed to sprintf file for config");
        return -1;
    }

    sret = snprintf(path, sizeof(path), "/v2/%s/blobs/%s", desc->name, desc->config.digest);
    if (sret < 0 || (size_t)sret >= sizeof(path)) {
        ERROR("Failed to sprintf path for config");
        ret = -1;
        goto out;
    }

    ret = fetch_data(desc, path, file, desc->config.media_type, desc->config.digest);
    if (ret != 0) {
        ERROR("registry: Get %s failed", path);
        goto out;
    }

    desc->config.file = util_strdup_s(file);

out:

    return ret;
}

int fetch_layer(pull_descriptor *desc, size_t index)
{
    int ret = 0;
    int sret = 0;
    char file[PATH_MAX] = { 0 };
    char path[PATH_MAX] = { 0 };
    layer_blob *layer = NULL;

    if (desc == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    if (index >= desc->layers_len) {
        ERROR("Invalid layer index %zu, total layer number %zu", index, desc->layers_len);
        return -1;
    }

    sret = snprintf(file, sizeof(file), "%s/%zu", desc->blobpath, index);
    if (sret < 0 || (size_t)sret >= sizeof(file)) {
        ERROR("Failed to sprintf file for layer %zu", index);
        return -1;
    }

    layer = &desc->layers[index];
    sret = snprintf(path, sizeof(path), "/v2/%s/blobs/%s", desc->name, layer->digest);
    if (sret < 0 || (size_t)sret >= sizeof(path)) {
        ERROR("Failed to sprintf path for layer %zu, name %s, digest %s", index, desc->name, layer->digest);
        ret = -1;
        goto out;
    }

    ret = fetch_data(desc, path, file, layer->media_type, layer->digest);
    if (ret != 0) {
        ERROR("registry: Get %s failed", path);
        goto out;
    }

out:

    return ret;
}

int parse_login(char *http_head, char *host)
{
    int ret = 0;
    struct parsed_http_message *message = NULL;

    message = get_parsed_message(http_head);
    if (message == NULL) {
        ERROR("Get parsed message failed. http response size %zu, response:%s", strlen(http_head), http_head);
        isulad_try_set_error_message("login to registry for %s failed: parse response failed", host);
        ret = -1;
        goto out;
    }

    if (message->status_code == StatusUnauthorized) {
        ERROR("login to registry for %s failed: invalid username/password", host);
        isulad_try_set_error_message("login to registry for %s failed: invalid username/password", host);
        ret = -1;
        goto out;
    }

    if (message->status_code != StatusOK) {
        ERROR("login to registry for %s failed: server response code %d", host, message->status_code);
        isulad_try_set_error_message("login to registry for %s failed: server response code %d", host,
                                     message->status_code);
        ret = -1;
        goto out;
    }

out:

    free_parsed_http_message(&message);

    return ret;
}

int login_to_registry(pull_descriptor *desc)
{
    int ret = 0;
    int sret = 0;
    char *resp_buffer = NULL;
    char path[PATH_MAX] = { 0 };
    CURLcode errcode = CURLE_OK;

    if (desc == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    sret = snprintf(path, sizeof(path), "/v2/");
    if (sret < 0 || (size_t)sret >= sizeof(path)) {
        ERROR("Failed to sprintf path for login");
        ret = -1;
        goto out;
    }

    ret = registry_request(desc, path, NULL, NULL, &resp_buffer, HEAD_BODY, &errcode);
    if (ret != 0) {
        ERROR("registry: Get %s failed, resp: %s", path, resp_buffer);
        isulad_try_set_error_message("login to registry for %s failed", desc->host);
        goto out;
    }

    ret = parse_login(resp_buffer, desc->host);
    if (ret != 0) {
        goto out;
    }

    ret = auths_save(desc->host, desc->username, desc->password);
    if (ret != 0) {
        ERROR("failed to save auth of host %s, use decrypted key %d", desc->host, desc->use_decrypted_key);
        isulad_try_set_error_message("save login result for %s failed", desc->host);
        goto out;
    }
out:

    free(resp_buffer);
    resp_buffer = NULL;

    return ret;
}
