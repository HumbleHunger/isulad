/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2018-2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: lifeng
 * Create: 2018-11-08
 * Description: provide container restful service functions
 ******************************************************************************/
#include "rest_service.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "isula_libutils/log.h"
#include "utils.h"
#include "rest_containers_service.h"
#include "rest_images_service.h"

#define REST_PTHREAD_NUM 100
#define BACKLOG 2048
static char *g_socketpath = NULL;
static evbase_t *g_evbase = NULL;
static evhtp_t *g_htp = NULL;

/* rest server free */
static void rest_server_free()
{
    if (g_socketpath != NULL) {
        free(g_socketpath);
        g_socketpath = NULL;
    }
    if (g_evbase != NULL) {
        event_base_free(g_evbase);
        g_evbase = NULL;
    }
    if (g_htp != NULL) {
        evhtp_free(g_htp);
        g_htp = NULL;
    }
}

/* rest register handler */
static int rest_register_handler(evhtp_t *g_htp)
{
    if (rest_register_containers_handler(g_htp) != 0) {
        return -1;
    }

    if (rest_register_images_handler(g_htp) != 0) {
        return -1;
    }

    return 0;
}

/* libevent log cb */
static void libevent_log_cb(int severity, const char *msg)
{
    switch (severity) {
        case EVENT_LOG_DEBUG:
            break;
        case EVENT_LOG_MSG:
            break;
        case EVENT_LOG_WARN:
            break;
        case EVENT_LOG_ERR:
            ERROR("libevent: %s", msg);
            break;
        default:
            FATAL("libevent: %s", msg);
            break;
    }
}

/* rest server init */
int rest_server_init(const char *socket)
{
    g_socketpath = util_strdup_s(socket);

    event_set_log_callback(libevent_log_cb);

    g_evbase = event_base_new();
    if (g_evbase == NULL) {
        ERROR("Failed to init rest server");
        goto error_out;
    }
    g_htp = evhtp_new(g_evbase, NULL);
    if (g_htp == NULL) {
        ERROR("Failed to init rest server");
        goto error_out;
    }

    if (unlink(g_socketpath + strlen(UNIX_SOCKET_PREFIX)) < 0 && errno != ENOENT) {
        ERROR("Failed to remove '%s':%s, abort", strerror(errno), g_socketpath);
        goto error_out;
    }

    if (rest_register_handler(g_htp) < 0) {
        ERROR("Register hanler failed");
        goto error_out;
    }

    evhtp_use_dynamic_threads(g_htp, NULL, NULL, 0, 0, 0, NULL);
    if (evhtp_bind_socket(g_htp, g_socketpath, 0, BACKLOG) < 0) {
        ERROR("Evhtp_bind_socket error");
        goto error_out;
    }

    return 0;

error_out:
    rest_server_free();
    return -1;
}

/* rest server wait */
void rest_server_wait(void)
{
    event_base_loop(g_evbase, 0);
}

/* rest server shutdown */
void rest_server_shutdown(void)
{
    if (g_socketpath != NULL) {
        if (unlink(g_socketpath + strlen(UNIX_SOCKET_PREFIX)) < 0 && errno != ENOENT) {
            ERROR("Failed to remove '%s':%s", g_socketpath, strerror(errno));
        }
    }
}

