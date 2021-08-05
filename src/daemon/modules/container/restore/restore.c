/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2017-2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: tanyifeng
 * Create: 2017-11-22
 * Description: provide container list callback function definition
 ********************************************************************************/
#include "restore.h"
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <isula_libutils/container_config_v2.h>
#include <isula_libutils/host_config.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "isulad_config.h"
#include "isula_libutils/log.h"
#include "container_api.h"
#include "supervisor.h"
#include "containers_gc.h"
#include "container_unix.h"
#include "image_api.h"
#include "runtime_api.h"
#include "service_container_api.h"
#include "restartmanager.h"
#include "constants.h"
#include "utils.h"
#include "utils_array.h"
#include "utils_file.h"
#include "utils_timestamp.h"

/* restore supervisor */
static int restore_supervisor(const container_t *cont)
{
    int ret = 0;
    int nret = 0;
    int exit_fifo_fd = -1;
    char container_state[PATH_MAX] = { 0 };
    char *exit_fifo = NULL;
    char *id = cont->common_config->id;
    char *statepath = cont->state_path;
    char *runtime = cont->runtime;
    pid_ppid_info_t pid_info = { 0 };

    nret = snprintf(container_state, sizeof(container_state), "%s/%s", statepath, id);
    if (nret < 0 || (size_t)nret >= sizeof(container_state)) {
        ERROR("Failed to sprintf container state %s/%s", statepath, id);
        ret = -1;
        goto out;
    }

    exit_fifo = exit_fifo_name(container_state);
    if (exit_fifo == NULL) {
        ERROR("Failed to get exit fifo name %s/%s", statepath, id);
        ret = -1;
        goto out;
    }

    exit_fifo_fd = container_exit_fifo_open(exit_fifo);
    if (exit_fifo_fd < 0) {
        ERROR("Failed to open exit FIFO %s", exit_fifo);
        ret = -1;
        goto out;
    }

    if (!util_process_alive(cont->state->state->pid, cont->state->state->start_time)) {
        ERROR("Container %s pid %d already dead, skip add supervisor", id, cont->state->state->pid);
        close(exit_fifo_fd);
        ret = -1;
        goto out;
    }

    pid_info.pid = cont->state->state->pid;
    pid_info.ppid = cont->state->state->p_pid;
    pid_info.start_time = cont->state->state->start_time;
    pid_info.pstart_time = cont->state->state->p_start_time;

    if (container_supervisor_add_exit_monitor(exit_fifo_fd, &pid_info, id, runtime)) {
        ERROR("Failed to add exit monitor to supervisor");
        ret = -1;
        goto out;
    }

out:
    free(exit_fifo);

    return ret;
}

/* post stopped container to gc */
static int post_stopped_container_to_gc(const char *id, const char *runtime, const char *statepath, uint32_t pid)
{
    int ret = 0;
    pid_ppid_info_t pid_info = { 0 };

    (void)util_read_pid_ppid_info(pid, &pid_info);

    if (gc_add_container(id, runtime, &pid_info)) {
        ERROR("Failed to post container %s to garbage collector", id);
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int check_container_image_exist(const container_t *cont)
{
    int ret = 0;
    char *tmp = NULL;
    const char *id = cont->common_config->id;
    const char *image_name = cont->common_config->image;
    const char *image_type = cont->common_config->image_type;

    if (image_type == NULL || image_name == NULL) {
        ERROR("Failed to get image type for container %s", id);
        ret = -1;
        goto out;
    }

    /* only check exist for oci image */
    if (strcmp(image_type, IMAGE_TYPE_OCI) == 0) {
        ret = im_resolv_image_name(image_type, image_name, &tmp);
        if (ret != 0) {
            ERROR("Failed to resolve image %s", image_name);
            goto out;
        }

        if (!im_oci_image_exist(tmp)) {
            WARN("Image %s not exist", tmp);
            ret = -1;
            goto out;
        }
    }

out:
    free(tmp);
    return ret;
}

static bool is_same_process(const container_t *cont, const pid_ppid_info_t *pid_info)
{
    if (pid_info->pid == cont->state->state->pid && pid_info->ppid == cont->state->state->p_pid &&
        pid_info->start_time == cont->state->state->start_time &&
        pid_info->pstart_time == cont->state->state->p_start_time) {
        return true;
    }
    return false;
}

static void try_to_set_paused_container_pid(Container_Status status, const container_t *cont,
                                            const pid_ppid_info_t *pid_info)
{
    if (status != CONTAINER_STATUS_PAUSED || !is_same_process(cont, pid_info)) {
        container_state_set_running(cont->state, pid_info, false);
    }
}

static void try_to_set_container_running(Container_Status status, container_t *cont, const pid_ppid_info_t *pid_info)
{
    if (status != CONTAINER_STATUS_RUNNING || !is_same_process(cont, pid_info)) {
        container_state_set_running(cont->state, pid_info, true);
    }
}

static void restore_stopped_container(Container_Status status, const container_t *cont)
{
    const char *id = cont->common_config->id;
    pid_t pid = 0;

    if (status != CONTAINER_STATUS_STOPPED && status != CONTAINER_STATUS_CREATED) {
        if (util_process_alive(cont->state->state->pid, cont->state->state->start_time)) {
            pid = cont->state->state->pid;
        }
        int nret = post_stopped_container_to_gc(id, cont->runtime, cont->state_path, pid);
        if (nret != 0) {
            ERROR("Failed to post container %s to garbage"
                  "collector, that may lost some resources"
                  "used with container!",
                  id);
        }
        container_state_set_stopped(cont->state, 255);
    }
}

static void restore_running_container(Container_Status status, container_t *cont,
                                      const struct runtime_container_status_info *info)
{
    int nret = 0;
    const char *id = cont->common_config->id;
    pid_ppid_info_t pid_info = { 0 };

    nret = util_read_pid_ppid_info(info->pid, &pid_info);
    if (nret == 0) {
        try_to_set_container_running(status, cont, &pid_info);
        container_state_reset_has_been_manual_stopped(cont->state);
    } else {
        ERROR("Failed to restore container:%s due to unable to read container pid information", id);
        nret = post_stopped_container_to_gc(id, cont->runtime, cont->state_path, 0);
        if (nret != 0) {
            ERROR("Failed to post container %s to garbage"
                  "collector, that may lost some resources"
                  "used with container!",
                  id);
        }
        container_state_set_stopped(cont->state, 255);
    }
}

static void restore_paused_container(Container_Status status, container_t *cont,
                                     const struct runtime_container_status_info *info)
{
    int nret = 0;
    const char *id = cont->common_config->id;
    pid_ppid_info_t pid_info = { 0 };

    container_state_set_paused(cont->state);

    nret = util_read_pid_ppid_info(info->pid, &pid_info);
    if (nret == 0) {
        try_to_set_paused_container_pid(status, cont, &pid_info);
        container_state_reset_has_been_manual_stopped(cont->state);
    } else {
        ERROR("Failed to restore container:%s due to unable to read container pid information", id);
        nret = post_stopped_container_to_gc(id, cont->runtime, cont->state_path, 0);
        if (nret != 0) {
            ERROR("Failed to post container %s to garbage"
                  "collector, that may lost some resources"
                  "used with container!",
                  id);
        }
        container_state_set_stopped(cont->state, 255);
    }
}

/* restore state */
static void restore_state(container_t *cont)
{
    int nret = 0;
    const char *id = cont->common_config->id;
    const char *runtime = cont->runtime;
    rt_status_params_t params = { 0 };
    struct runtime_container_status_info real_status = { 0 };
    Container_Status status = container_state_get_status(cont->state);

    (void)container_exit_on_next(cont); /* cancel restart policy */

    params.rootpath = cont->root_path;
    params.state = cont->state_path;
    nret = runtime_status(id, runtime, &params, &real_status);
    if (nret != 0) {
        ERROR("Failed to restore container %s, make real status to STOPPED. Due to can not load container with status %d",
              id, status);
        real_status.status = RUNTIME_CONTAINER_STATUS_STOPPED;
    }

    if (real_status.status == RUNTIME_CONTAINER_STATUS_STOPPED) {
        restore_stopped_container(status, cont);
    } else if (real_status.status == RUNTIME_CONTAINER_STATUS_RUNNING) {
        restore_running_container(status, cont, &real_status);
    } else if (real_status.status == RUNTIME_CONTAINER_STATUS_PAUSED) {
        restore_paused_container(status, cont, &real_status);
    } else {
        ERROR("Container %s get invalid status %d", id, real_status.status);
    }

    if (container_is_removal_in_progress(cont->state)) {
        container_state_reset_removal_in_progress(cont->state);
    }
    if (container_state_to_disk(cont) != 0) {
        ERROR("Failed to re-save container \"%s\" to disk", id);
    }
}

/* remove invalid container */
static int remove_invalid_container(const container_t *cont, const char *runtime, const char *root, const char *state,
                                    const char *id)
{
    int ret = 0;
    char container_root[PATH_MAX] = { 0x00 };
    char container_state[PATH_MAX] = { 0x00 };

    ret = snprintf(container_state, sizeof(container_state), "%s/%s", state, id);
    if (ret < 0 || (size_t)ret >= sizeof(container_state)) {
        ERROR("Failed to sprintf container state %s/%s", state, id);
        ret = -1;
        goto out;
    }
    ret = util_recursive_rmdir(container_state, 0);
    if (ret != 0) {
        ERROR("Failed to delete container's state directory %s", container_state);
        ret = -1;
        goto out;
    }

    ret = cleanup_mounts_by_id(id, root);
    if (ret != 0) {
        ERROR("Failed to clean container's mounts");
        ret = -1;
        goto out;
    }

    ret = snprintf(container_root, sizeof(container_root), "%s/%s", root, id);
    if (ret < 0 || (size_t)ret >= sizeof(container_root)) {
        ERROR("Failed to sprintf invalid root directory %s/%s", root, id);
        ret = -1;
        goto out;
    }

    if (cont != NULL && im_remove_container_rootfs(cont->common_config->image_type, id)) {
        ERROR("Failed to remove rootfs for container %s", id);
        ret = -1;
        goto out;
    }

    ret = util_recursive_rmdir(container_root, 0);
    if (ret != 0) {
        ERROR("Failed to delete container's state directory %s", container_state);
        ret = -1;
        goto out;
    }
out:
    return ret;
}

static void restored_restart_container(container_t *cont)
{
    char *id = NULL;
    char *started_at = NULL;
    uint64_t timeout = 0;

    id = cont->common_config->id;

    started_at = container_state_get_started_at(cont->state);
    if (restart_manager_should_restart(id, container_state_get_exitcode(cont->state),
                                       container_state_get_has_been_manual_stopped(cont->state),
                                       util_time_seconds_since(started_at), &timeout)) {
        container_state_increase_restart_count(cont->state);
        INFO("Restart container %s after 5 second", id);
        (void)container_restart_in_thread(id, 5ULL * Time_Second, (int)container_state_get_exitcode(cont->state));
    }
    free(started_at);
}

/* handle restored container */
static void handle_restored_container()
{
    int ret = 0;
    size_t i = 0;
    size_t container_num = 0;
    char *id = NULL;
    container_t **conts = NULL;
    container_t *cont = NULL;

    ret = containers_store_list(&conts, &container_num);
    if (ret != 0) {
        ERROR("query all containers info failed");
        return;
    }

    for (i = 0; i < container_num; i++) {
        cont = conts[i];
        container_lock(cont);

        (void)container_reset_restart_manager(cont, false);

        id = cont->common_config->id;

        if (container_is_in_gc_progress(id)) {
            ERROR("Container %s is in gc process, skip it in restore process", id);
            goto unlock_continue;
        }

        if (container_is_running(cont->state)) {
            if (restore_supervisor(cont) != 0) {
                ERROR("Failed to restore %s supervisor, set state to stopped", id);
                container_state_set_stopped(cont->state, 255);
                if (post_stopped_container_to_gc(id, cont->runtime, cont->state_path, 0) != 0) {
                    ERROR("Failed to post container %s to garbage"
                          "collector, that may lost some resources"
                          "used with container!",
                          id);
                }
                goto unlock_continue;
            }
            container_init_health_monitor(id);
        } else {
            if (cont->hostconfig != NULL && cont->hostconfig->auto_remove_bak) {
                (void)set_container_to_removal(cont);
                container_unlock(cont);
                (void)delete_container(cont, true);
                container_lock(cont);
            } else {
                restored_restart_container(cont);
            }
        }

unlock_continue:
        container_unlock(cont);
        container_unref(cont);
    }

    free(conts);
    return;
}

/* scan dir to add store */
static void scan_dir_to_add_store(const char *runtime, const char *rootpath, const char *statepath,
                                  const size_t subdir_num, const char **subdir)
{
    size_t i = 0;
    container_t *cont = NULL;

    for (i = 0; i < subdir_num; i++) {
        cont = NULL;
        bool aret = false;
        bool index_flag = false;
        cont = container_load(runtime, rootpath, statepath, subdir[i]);
        if (cont == NULL) {
            ERROR("Failed to load subdir:%s", subdir[i]);
            goto error_load;
        }

        if (check_container_image_exist(cont) != 0) {
            ERROR("Failed to restore container:%s due to image not exist", subdir[i]);
            goto error_load;
        }

        restore_state(cont);

        index_flag = container_name_index_add(cont->common_config->name, cont->common_config->id);
        if (!index_flag) {
            ERROR("Failed add %s into name indexs", subdir[i]);
            goto error_load;
        }
        aret = containers_store_add(cont->common_config->id, cont);
        if (!aret) {
            ERROR("Failed add container %s to store", subdir[i]);
            goto error_load;
        }

        continue;
error_load:
        if (remove_invalid_container(cont, runtime, rootpath, statepath, subdir[i])) {
            ERROR("Failed to delete subdir:%s", subdir[i]);
        }

        if (index_flag) {
            container_name_index_remove(cont->common_config->name);
        }
        container_unref(cont);
        continue;
    }
}

/* restore container by runtime */
static int restore_container_by_runtime(const char *runtime)
{
    int ret = 0;
    char *rootpath = NULL;
    char *statepath = NULL;
    size_t subdir_num = 0;
    char **subdir = NULL;

    rootpath = conf_get_routine_rootdir(runtime);
    if (rootpath == NULL) {
        ERROR("Root path is NULL");
        ret = -1;
        goto out;
    }

    statepath = conf_get_routine_statedir(runtime);
    if (statepath == NULL) {
        ERROR("State path is NULL");
        ret = -1;
        goto out;
    }

    ret = util_list_all_subdir(rootpath, &subdir);
    if (ret != 0) {
        ERROR("Failed to read %s'subdirectory", rootpath);
        ret = -1;
        goto out;
    }
    subdir_num = util_array_len((const char **)subdir);
    if (subdir_num == 0) {
        goto out;
    }

    scan_dir_to_add_store(runtime, rootpath, statepath, subdir_num, (const char **)subdir);

out:
    free(rootpath);
    free(statepath);
    util_free_array(subdir);
    return ret;
}

/* containers restore */
void containers_restore(void)
{
    int ret = 0;
    size_t subdir_num = 0;
    size_t i = 0;
    char *engines_path = NULL;
    char **subdir = NULL;

    engines_path = conf_get_engine_rootpath();
    if (engines_path == NULL) {
        ERROR("Failed to get engines path");
        goto out;
    }

    ret = util_list_all_subdir(engines_path, &subdir);
    if (ret != 0) {
        ERROR("Failed to list engines");
        goto out;
    }
    subdir_num = util_array_len((const char **)subdir);

    for (i = 0; i < subdir_num; i++) {
        DEBUG("Restore the containers by runtime:%s", subdir[i]);
        ret = restore_container_by_runtime(subdir[i]);
        if (ret != 0) {
            ERROR("Failed to restore containers by runtime:%s", subdir[i]);
        }
    }

    handle_restored_container();

out:
    free(engines_path);
    util_free_array(subdir);
    return;
}
