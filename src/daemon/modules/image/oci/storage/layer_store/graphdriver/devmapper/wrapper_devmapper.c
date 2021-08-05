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
* Author: gaohuatao
* Create: 2020-01-19
* Description: wrap libdevmapper function to manuplite devicemapper
******************************************************************************/
#define _GNU_SOURCE
#include "wrapper_devmapper.h"
#include <sys/time.h>
#include <stdio.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#include "isula_libutils/log.h"
#include "utils.h"
#include "err_msg.h"

static bool g_dm_saw_busy = false;
static bool g_dm_saw_exist = false;
static bool g_dm_saw_enxio = false; // no such device or address
static bool g_dm_saw_eno_data = false; // no data available
static int64_t dm_udev_wait_timeout = 0;

char *dev_strerror(int errnum)
{
    char *errmsg = NULL;

    switch (errnum) {
        case ERR_TASK_RUN:
            errmsg = "Task run error";
            break;
        case ERR_TASK_SET_COOKIE:
            errmsg = "Task set cookie error";
            break;
        case ERR_NIL_COOKIE:
            errmsg = "cookie ptr can't be nil";
            break;
        case ERR_TASK_SET_ADD_NODE:
            errmsg = "Task add dm node failed";
            break;
        case ERR_BUSY:
            errmsg = "Device busy";
            break;
        case ERR_DEVICE_ID_EXISTS:
            errmsg = "Device exists already";
            break;
        case ERR_ENXIO:
            errmsg = "No such device of address";
            break;
        case ERR_TASK_ADD_TARGET:
            errmsg = "Task add target device error";
            break;
        case ERR_TASK_DEFERRED_REMOVE:
            errmsg = "dm_task_deferred_remove failed";
            break;
        default:
            errmsg = "Unknown error";
            break;
    }
    return errmsg;
}

struct dm_task *task_create(int type)
{
    struct dm_task *dmt = NULL;

    dmt = dm_task_create(type);
    return dmt;
}

int set_message(struct dm_task *dmt, const char *message)
{
    if (dmt == NULL || message == NULL) {
        ERROR("invalid argument");
        return -1;
    }

    return dm_task_set_message(dmt, message) != 1 ? -1 : 0;
}

int set_sector(struct dm_task *dmt, uint64_t sector)
{
    int ret = 0;

    if (dmt == NULL) {
        ERROR("invalid argument");
        return -1;
    }

    if (dm_task_set_sector(dmt, sector) != 1) {
        ERROR("devicemapper; set sectot failed");
        ret = -1;
    }

    return ret;
}

int set_add_node(struct dm_task *dmt, dm_add_node_t add_node)
{
    int ret = 0;

    if (dmt == NULL) {
        ERROR("invalid argument");
        return -1;
    }

    if (add_node != DM_ADD_NODE_ON_RESUME && add_node != DM_ADD_NODE_ON_CREATE) {
        ret = ERR_INVALID_ADD_NODE;
        goto out;
    }

    if (dm_task_set_add_node(dmt, add_node) != 1) {
        ret = ERR_TASK_SET_ADD_NODE;
        goto out;
    }

out:
    return ret;
}

static int add_target(struct dm_task *dmt, uint64_t start, uint64_t size, const char *ttype, const char *params)
{
    if (dmt == NULL || ttype == NULL || params == NULL) {
        ERROR("devicemapper: invalid input params to add target");
        return -1;
    }

    if (dm_task_add_target(dmt, start, size, ttype, params) != 1) {
        ERROR("devmapper:dm task add target failed, params is %s", params);
        return ERR_TASK_ADD_TARGET;
    }

    return 0;
}

void set_udev_wait_timeout(int64_t t)
{
    dm_udev_wait_timeout = t;
}

int set_dev_dir(const char *dir)
{
    int ret = 0;

    if (dir == NULL) {
        ERROR("invalid argument");
        return -1;
    }

    if (dm_set_dev_dir(dir) != 1) {
        ret = -1;
    }

    return ret;
}

struct dm_task *task_create_named(int type, const char *dm_name)
{
    struct dm_task *dmt = NULL;

    if (dm_name == NULL) {
        ERROR("devicemapper: invalid input");
        return NULL;
    }

    dmt = dm_task_create(type);
    if (dmt == NULL) {
        ERROR("devicemapper: Can't create task of type %d", type);
        return NULL;
    }

    if (dm_task_set_name(dmt, dm_name) != 1) {
        ERROR("devicemapper: Can't set task name %s", dm_name);
        goto cleanup;
    }

    return dmt;

cleanup:
    dm_task_destroy(dmt);
    return NULL;
}

char *dev_get_driver_version()
{
    struct dm_task *dmt = NULL;
    char *version = NULL;
    size_t size = 128;

    version = util_common_calloc_s(size);
    if (version == NULL) {
        ERROR("devmapper: out of memory");
        return NULL;
    }

    dmt = task_create(DM_DEVICE_VERSION);
    if (dmt == NULL) {
        ERROR("devicemapper: create task DM_DEVICE_VERSION failed");
        goto err_out;
    }

    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        goto err_out;
    }

    if (dm_task_get_driver_version(dmt, version, size) == 0) {
        ERROR("devicemapper; get driver version failed");
        goto err_out;
    }

    goto cleanup;

err_out:
    free(version);
    version = NULL;

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return version;
}

// dev_get_library_version return the device mapper library version
char *dev_get_library_version()
{
    char version[128] = { 0 };

    if (dm_get_library_version(version, sizeof(version)) != 1) {
        ERROR("dm_get_library_version failed");
        return NULL;
    }

    return util_strdup_s(version);
}

// GetStatus is the programmatic example of "dmsetup status".
// It outputs status information for the specified device name.
int dev_get_status(uint64_t *start, uint64_t *length, char **target_type, char **params, const char *name)
{
    int ret = 0;
    struct dm_task *dmt = NULL;
    struct dm_info info;
    uint64_t dm_length = 0;
    uint64_t dm_start = 0;
    char *dm_target_type = NULL;
    char *dm_params = NULL;

    if (start == NULL || length == NULL || target_type == NULL || params == NULL || name == NULL) {
        ERROR("devicemapper: invalid input params to get table");
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_STATUS, name);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_STATUS) failed");
        ret = -1;
        goto cleanup;
    }

    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        ret = -1;
        goto cleanup;
    }

    if (dm_task_get_info(dmt, &info) != 1) {
        ERROR("devicemapper: get info err");
        ret = -1;
        goto cleanup;
    }

    if (info.exists == 0) {
        ERROR("devicemapper: GetTable() Non existing device %s", name);
        ret = -1;
        goto cleanup;
    }

    (void)dm_get_next_target(dmt, NULL, &dm_start, &dm_length, &dm_target_type, &dm_params);
    *start = dm_start;
    *length = dm_length;
    *target_type = util_strdup_s(dm_target_type);
    *params = util_strdup_s(dm_params);

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

int dev_get_info(struct dm_info *info, const char *name)
{
    int ret = 0;
    struct dm_task *dmt = NULL;

    if (info == NULL || name == NULL) {
        ERROR("devicemapper: invalid input params to get info");
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_INFO, name);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_INFO) failed");
        ret = -1;
        goto cleanup;
    }

    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        ret = -1;
        goto cleanup;
    }

    if (dm_task_get_info(dmt, info) != 1) {
        ERROR("devicemapper: get info err");
        ret = -1;
        goto cleanup;
    }

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

static int set_cookie(struct dm_task *dmt, uint32_t *cookie, uint16_t flags)
{
    int ret = 0;

    if (cookie == NULL) {
        ERROR("cookie ptr can't be nil");
        ret = ERR_NIL_COOKIE;
        goto out;
    }

    if (dm_task_set_cookie(dmt, cookie, flags) != 1) {
        ERROR("dm_task_set_cookie failed");
        ret = ERR_TASK_SET_COOKIE;
        goto out;
    }

out:
    return ret;
}

static void *udev_wait_process(void *data)
{
    int ret = 0;
    udev_wait_pth_t *uwait = (udev_wait_pth_t *)data;

    if (pthread_detach(pthread_self()) != 0) {
        CRIT("Start: set thread detach fail");
        goto out;
    }

    ret = dm_udev_wait(uwait->cookie);
    pthread_mutex_lock(&uwait->udev_mutex);
    if (ret != 1) {
        uwait->state = ERR_UDEV_WAIT;
    } else {
        uwait->state = DEV_OK;
    }
    pthread_mutex_unlock(&uwait->udev_mutex);

out:
    DAEMON_CLEAR_ERRMSG();
    return NULL;
}

// UdevWait waits for any processes that are waiting for udev to complete the specified cookie.
void dev_udev_wait(uint32_t cookie)
{
    pthread_t tid;
    udev_wait_pth_t *uwait = NULL;
    float timeout = 0;
    struct timeval start, end;

    if (gettimeofday(&start, NULL) != 0) {
        ERROR("devmapper: get time failed");
        goto free_out;
    }

    uwait = util_common_calloc_s(sizeof(udev_wait_pth_t));
    if (uwait == NULL) {
        ERROR("Out of memory");
        goto free_out;
    }
    uwait->cookie = cookie;
    uwait->state = DEV_INIT;

    if (pthread_mutex_init(&uwait->udev_mutex, NULL) != 0) {
        ERROR("Udev mutex initialized failed");
        goto free_out;
    }

    if (pthread_create(&tid, NULL, udev_wait_process, uwait) != 0) {
        ERROR("devmapper: create udev wait process thread error:%s", strerror(errno));
        goto free_out;
    }

    while (true) {
        pthread_mutex_lock(&uwait->udev_mutex);
        if (uwait->state != DEV_INIT) {
            pthread_mutex_unlock(&uwait->udev_mutex);
            goto free_out;
        }
        pthread_mutex_unlock(&uwait->udev_mutex);

        if (gettimeofday(&end, NULL) != 0) {
            ERROR("devmapper: get time failed");
            goto free_out;
        }

        timeout = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000; // seconds
        if (timeout >= (float)dm_udev_wait_timeout) {
            if (dm_udev_complete(cookie) != 1) {
                ERROR("Failed to complete udev cookie %u on udev wait timeout", cookie);
                goto free_out;
            }
            ERROR("Wait on udev cookie time out");
            break;
        }
    }

free_out:
    pthread_mutex_destroy(&uwait->udev_mutex);
    free(uwait);
}

int dev_delete_device_force(const char *name)
{
    int ret = 0;
    int nret = 0;
    struct dm_task *dmt = NULL;
    uint32_t cookie = 0;

    if (name == NULL) {
        ERROR("invalid argument");
        ret = -1;
        goto out;
    }

    dmt = task_create_named(DM_DEVICE_REMOVE, name);
    if (dmt == NULL) {
        ERROR("devicemapper: create task with name:DM_DEVICE_REMOVE failed");
        ret = -1;
        goto out;
    }

    nret = set_cookie(dmt, &cookie, 0);
    if (nret != 0) {
        ERROR("set cookie failed:%s", dev_strerror(nret));
        ret = -1;
        goto out;
    }

    g_dm_saw_busy = false;
    g_dm_saw_enxio = false;
    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        if (g_dm_saw_busy) {
            ERROR("devicemapper: delete task run err type is \"device is busy\"");
            ret = ERR_BUSY;
            goto udev;
        }

        if (g_dm_saw_enxio) {
            ERROR("devicemapper: delete task run err type is \"No such device or address\"");
            ret = ERR_ENXIO;
            goto udev;
        }
        ret = -1;
        goto udev;
    }

udev:
    DEBUG("Start udev wait on delete device force");
    dev_udev_wait(cookie);

out:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

int dev_remove_device_deferred(const char *name)
{
    int ret = 0;
    int nret = 0;
    struct dm_task *dmt = NULL;
    uint32_t cookie = 0;
    uint16_t flags = DM_UDEV_DISABLE_LIBRARY_FALLBACK;

    if (name == NULL) {
        ERROR("invalid argument");
        ret = -1;
        goto out;
    }

    dmt = task_create_named(DM_DEVICE_REMOVE, name);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_REMOVE) failed");
        ret = -1;
        goto out;
    }

    if (dm_task_deferred_remove(dmt) != 1) {
        ERROR("devicemapper: do task deferred remove failed");
        ret = ERR_TASK_DEFERRED_REMOVE;
        goto out;
    }

    nret = set_cookie(dmt, &cookie, flags);
    if (nret != 0) {
        ERROR("set cookie failed:%s", dev_strerror(nret));
        ret = -1;
        goto out;
    }

    g_dm_saw_enxio = false;
    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        if (g_dm_saw_enxio) {
            ERROR("devicemapper: delete deferred task run err type is \"No such device or address\"");
            ret = ERR_ENXIO;
            goto udev;
        }
        ret = ERR_TASK_RUN;
        goto udev;
    }

udev:
    DEBUG("Start udev wait on remove device deferred");
    dev_udev_wait(cookie);
out:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

static char **local_dm_task_get_names(struct dm_task *dmt, size_t *size)
{
    struct dm_names *ns, *ns1;
    unsigned next = 0;
    char **result;
    int i = 0;

    if (!(ns = dm_task_get_names(dmt))) {
        ERROR("Failed to get device names list  from dm task");
        return NULL;
    }

    // No devices found
    if (!ns->dev) {
        return NULL;
    }

    // calucate the total devices
    ns1 = ns;
    *size = 0;
    do {
        ns1 = (struct dm_names *)((char *)ns1 + next);
        (*size)++;
        next = ns1->next;
    } while (next);

    result = malloc(sizeof(char *) * (*size));
    if (!result) {
        ERROR("Out of memory");
        return NULL;
    }

    next = 0;
    do {
        ns = (struct dm_names *)((char *)ns + next);
        result[i++] = strdup(ns->name);
        next = ns->next;
    } while (next);

    return result;
}

int dev_get_device_list(char ***list, size_t *length)
{
    int ret = 0;
    struct dm_task *dmt = NULL;

    if (list == NULL || length == NULL) {
        ERROR("Invalid arguments");
        return -1;
    }

    dmt = task_create(DM_DEVICE_LIST);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task status failed");
        ret = -1;
        goto cleanup;
    }

    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        ret = -1;
        goto cleanup;
    }

    *list = local_dm_task_get_names(dmt, length);
    if (*list == NULL) {
        *length = 0;
        ERROR("devicemapper: get device  list empty");
        ret = -1;
        goto cleanup;
    }

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

bool udev_sync_supported()
{
    return dm_udev_get_sync_support() != 0;
}

bool udev_set_sync_support(bool enable)
{
    int enable_sync = 1;
    int unenable_sync = 0;

    if (enable) {
        dm_udev_set_sync_support(enable_sync);
    } else {
        dm_udev_set_sync_support(unenable_sync);
    }

    return udev_sync_supported();
}

// poolName : /dev/mapper/thin-pool
// CreateDevice creates a device with the specified poolName with the specified device id.
int dev_create_device(const char *pool_fname, int device_id)
{
    int ret = 0;
    int nret = 0;
    uint64_t sector = 0;
    char message[PATH_MAX] = { 0 }; // 临时字符缓冲区上限
    struct dm_task *dmt = NULL;

    if (pool_fname == NULL) {
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_TARGET_MSG, pool_fname);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_TARGET_MSG) failed");
        ret = -1;
        goto cleanup;
    }

    if (set_sector(dmt, sector) != 0) {
        ERROR("devicemapper: Can't set sector");
        ret = -1;
        goto cleanup;
    }

    nret = snprintf(message, sizeof(message), "create_thin %d", device_id);
    if (nret < 0 || (size_t)nret >= sizeof(message)) {
        ERROR("Print message create_thin %d failed", device_id);
        ret = -1;
        goto cleanup;
    }

    if (set_message(dmt, message) != 0) {
        ret = -1;
        goto cleanup;
    }

    g_dm_saw_exist = false;
    if (dm_task_run(dmt) != 1) {
        if (g_dm_saw_exist) {
            ret = ERR_DEVICE_ID_EXISTS;
        } else {
            ret = -1;
        }
        ERROR("devicemapper: task run failed");
        goto cleanup;
    }

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

int dev_delete_device(const char *pool_fname, int device_id)
{
    int ret = 0;
    int nret = 0;
    uint64_t sector = 0;
    char message[PATH_MAX] = { 0 };
    struct dm_task *dmt = NULL;

    if (pool_fname == NULL) {
        ERROR("devicemapper: pool full name is NULL");
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_TARGET_MSG, pool_fname);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_TARGET_MSG) failed");
        ret = -1;
        goto cleanup;
    }

    if (set_sector(dmt, sector) != 0) {
        ERROR("devicemapper: Can't set sector");
        ret = -1;
        goto cleanup;
    }

    nret = snprintf(message, sizeof(message), "delete %d", device_id);
    if (nret < 0 || (size_t)nret >= sizeof(message)) {
        ret = -1;
        goto cleanup;
    }

    if (set_message(dmt, message) != 0) {
        ret = -1;
        goto cleanup;
    }

    g_dm_saw_busy = false;
    g_dm_saw_eno_data = false;
    if (dm_task_run(dmt) != 1) {
        if (g_dm_saw_busy) {
            ret = ERR_BUSY;
            ERROR("devicemapper: Error delete device:device is busy");
            goto cleanup;
        }
        if (g_dm_saw_eno_data) {
            DEBUG("devicemapper: device(id:%d) from pool(%s) does not exist", device_id, pool_fname);
            goto cleanup;
        }
        ERROR("devicemapper: task run failed");
        ret = -1;
        goto cleanup;
    }

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

// SuspendDevice is the programmatic example of "dmsetup suspend".
// It suspends the specified device.
int dev_suspend_device(const char *dm_name)
{
    int ret = 0;
    struct dm_task *dmt = NULL;

    if (dm_name == NULL) {
        ERROR("devicemapper: invalid input param to suspend device");
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_SUSPEND, dm_name);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_SUSPEND) failed");
        ret = -1;
        goto out;
    }

    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        ret = -1;
        goto out;
    }

out:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

// ResumeDevice is the programmatic example of "dmsetup resume".
// It un-suspends the specified device.
void dev_resume_device(const char *dm_name)
{
    int nret = 0;
    uint32_t cookie = 0;
    uint16_t flags = 0;
    struct dm_task *dmt = NULL;

    if (dm_name == NULL) {
        ERROR("devicemapper: invalid input params to resume device");
        return;
    }

    dmt = task_create_named(DM_DEVICE_RESUME, dm_name);
    if (dmt == NULL) {
        ERROR("devicemapper: create named task(DM_DEVICE_RESUME) failed");
        goto out;
    }

    nret = set_cookie(dmt, &cookie, flags);
    if (nret != 0) {
        ERROR("set cookie failed:%s", dev_strerror(nret));
        goto out;
    }

    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: run task of DM_DEVICE_RESUME failed");
    }

    DEBUG("Start udev wait on resume device");
    dev_udev_wait(cookie);

out:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
}

int dev_active_device(const char *pool_name, const char *name, int device_id, uint64_t size)
{
    int ret = 0;
    int nret = 0;
    uint64_t start = 0;
    uint32_t cookie = 0;
    uint16_t flags = 0;
    char params[PATH_MAX] = { 0 };
    struct dm_task *dmt = NULL;
    dm_add_node_t add_node_type = DM_ADD_NODE_ON_CREATE;

    if (pool_name == NULL || name == NULL) {
        ERROR("devicemapper: invalid input params to active device");
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_CREATE, name);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_CREATE) failed");
        ret = -1;
        goto out;
    }

    nret = snprintf(params, sizeof(params), "%s %d", pool_name, device_id);
    if (nret < 0 || (size_t)nret >= sizeof(params)) {
        ERROR("Print params with pool name:%s, device_id:%d failed", pool_name, device_id);
        ret = -1;
        goto out;
    }

    if (add_target(dmt, start, size / 512, "thin", params) != 0) {
        ERROR("devicemapper: Can't add target");
        ret = -1;
        goto out;
    }

    if (set_add_node(dmt, add_node_type) != 0) {
        ERROR("devicemapper: Can't add node");
        ret = -1;
        goto out;
    }

    nret = set_cookie(dmt, &cookie, flags);
    if (nret != 0) {
        ERROR("set cookie failed:%s", dev_strerror(nret));
        ret = -1;
        goto out;
    }

    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        ret = -1;
    }

    DEBUG("Start udev wait on create device");
    dev_udev_wait(cookie);
out:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

int dev_cancel_deferred_remove(const char *dm_name)
{
    int ret = 0;
    uint64_t sector = 0;
    struct dm_task *dmt = NULL;

    if (dm_name == NULL) {
        ERROR("devicemapper: invalid dm name to cancel deferred remove");
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_TARGET_MSG, dm_name);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_TARGET_MSG) failed");
        ret = -1;
        goto cleanup;
    }

    if (set_sector(dmt, sector) != 0) {
        ERROR("devicemapper: Can't set sector");
        ret = -1;
        goto cleanup;
    }

    if (set_message(dmt, "@cancel_deferred_remove") != 0) {
        ret = -1;
        goto cleanup;
    }

    g_dm_saw_busy = false;
    g_dm_saw_enxio = false;
    if (dm_task_run(dmt) != 1) {
        if (g_dm_saw_busy) {
            ERROR("devicemapper: Error delete device:device is busy");
            ret = ERR_BUSY;
            goto cleanup;
        }
        if (g_dm_saw_enxio) {
            ERROR("devicemapper: Error delete device:no such device or address");
            ret = ERR_ENXIO;
            goto cleanup;
        }
        ERROR("devicemapper: task run failed");
        ret = -1;
        goto cleanup;
    }

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

// DMLog is the logging callback containing all of the information from devicemapper.
static void dm_log(int level, char *file, int line, int dm_errno_or_class, char *message)
{
    switch (level) {
        case LOG_LEVEL_FATAL:
        case LOG_LEVEL_ERR:
            ERROR("libdevmapper(%d): %s:%d (%d) %s", level, file, line, dm_errno_or_class, message);
            break;
        case LOG_LEVEL_WARN:
            WARN("libdevmapper(%d): %s:%d (%d) %s", level, file, line, dm_errno_or_class, message);
            break;
        case LOG_LEVEL_NOTICE:
        case LOG_LEVEL_INFO:
            INFO("libdevmapper(%d): %s:%d (%d) %s", level, file, line, dm_errno_or_class, message);
            break;
        case LOG_LEVEL_DEBUG:
            DEBUG("libdevmapper(%d): %s:%d (%d) %s", level, file, line, dm_errno_or_class, message);
            break;
        default:
            INFO("libdevmapper(%d): %s:%d (%d) %s", level, file, line, dm_errno_or_class, message);
    }
}

void storage_devmapper_log_callback(int level, char *file, int line, int dm_errno_or_class, char *message)
{
    if (level < LOG_LEVEL_DEBUG) {
        if (strstr(message, "busy") != NULL) {
            g_dm_saw_busy = true;
        }
        if (strstr(message, "File exist") != NULL) {
            g_dm_saw_exist = true;
        }

        if (strstr(message, "No such device or address") != NULL) {
            g_dm_saw_enxio = true;
        }

        if (strstr(message, "No data available") != NULL) {
            g_dm_saw_eno_data = true;
        }
    }
    dm_log(level, file, line, dm_errno_or_class, message);
}

static void log_cb(int level, const char *file, int line, int dm_errno_or_class, const char *f, ...)
{
    char *buffer = NULL;
    va_list ap;
    int nret = 0;

    va_start(ap, f);
    nret = vasprintf(&buffer, f, ap);
    va_end(ap);
    if (nret < 0) {
        // memory allocation failed -- should never happen?
        return;
    }

    storage_devmapper_log_callback(level, (char *)file, line, dm_errno_or_class, buffer);
    free(buffer);
}

void log_with_errno_init()
{
    dm_log_with_errno_init(log_cb);
}

// CreateSnapDeviceRaw creates a snapshot device. Caller needs to suspend and resume the origin device if it is active.
int dev_create_snap_device_raw(const char *pool_name, int device_id, int base_device_id)
{
    int ret = 0;
    int nret = 0;
    uint64_t sector = 0;
    char message[PATH_MAX] = { 0 };
    struct dm_task *dmt = NULL;

    if (pool_name == NULL) {
        ERROR("devicemapper: invalid input param to create snap device");
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_TARGET_MSG, pool_name);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task(DM_DEVICE_TARGET_MSG) failed");
        ret = -1;
        goto cleanup;
    }

    if (set_sector(dmt, sector) != 0) {
        ERROR("devicemapper: Can't set sector");
        ret = -1;
        goto cleanup;
    }

    nret = snprintf(message, sizeof(message), "create_snap %d %d", device_id, base_device_id);
    if (nret < 0 || (size_t)nret >= sizeof(message)) {
        ERROR("devicemapper: print create_snap message failed");
        ret = -1;
        goto cleanup;
    }

    if (set_message(dmt, message) != 0) {
        ERROR("devicemapper: Can't set message %s", message);
        ret = -1;
        goto cleanup;
    }

    g_dm_saw_exist = false;
    if (dm_task_run(dmt) != 1) {
        if (g_dm_saw_exist) {
            ERROR("devicemapper: Error create snap:device id not exists");
            ret = ERR_DEVICE_ID_EXISTS;
            goto cleanup;
        }
        ERROR("devicemapper: task run failed");
        ret = -1;
        goto cleanup;
    }

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

// SetTransactionID sets a transaction id for the specified device name.
int dev_set_transaction_id(const char *pool_name, uint64_t old_id, uint64_t new_id)
{
    int ret = 0;
    int nret = 0;
    uint64_t sector = 0;
    char message[PATH_MAX] = { 0 }; // 临时字符缓冲区上限
    struct dm_task *dmt = NULL;

    if (pool_name == NULL) {
        ERROR("devicemapper: pool full name is NULL");
        return -1;
    }

    dmt = task_create_named(DM_DEVICE_TARGET_MSG, pool_name);
    if (dmt == NULL) {
        ERROR("devicemapper:create named task %s failed", pool_name);
        ret = -1;
        goto cleanup;
    }

    if (set_sector(dmt, sector) != 0) {
        ERROR("devicemapper: Can't set sector");
        ret = -1;
        goto cleanup;
    }

    nret = snprintf(message, sizeof(message), "set_transaction_id %lu %lu", old_id, new_id);
    if (nret < 0 || (size_t)nret >= sizeof(message)) {
        ERROR("devicemapper:print set_transaction_id message failed");
        ret = -1;
        goto cleanup;
    }

    if (set_message(dmt, message) != 0) {
        ret = -1;
        goto cleanup;
    }

    if (dm_task_run(dmt) != 1) {
        ERROR("devicemapper: task run failed");
        ret = -1;
        goto cleanup;
    }

cleanup:
    if (dmt != NULL) {
        dm_task_destroy(dmt);
    }
    return ret;
}

void dev_check_sem_set_stat(int *semusz, int *semmni)
{
    struct seminfo sinfo;

    if (semusz == NULL || semmni == NULL) {
        return;
    }

    if (semctl(0, 0, SEM_INFO, &sinfo) != 0) {
        WARN("Get devmapper library version err:%s", strerror(errno));
        return;
    }

    *semusz = sinfo.semusz;
    *semmni = sinfo.semmni;
}
