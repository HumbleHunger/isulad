/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: lifeng
 * Create: 2020-04-02
 * Description: provide overlay2 function definition
 ******************************************************************************/
#include "driver_overlay2.h"

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mount.h>
#include <errno.h>
#include <stdio.h>
#include <strings.h>

#include "isula_libutils/log.h"
#include "isulad_config.h"
#include "path.h"
#include "utils.h"
#include "util_archive.h"
#include "project_quota.h"
#include "driver.h"
#include "driver_overlay2_types.h"
#include "image_api.h"
#include "utils_array.h"
#include "utils_convert.h"
#include "utils_file.h"
#include "utils_fs.h"
#include "utils_string.h"
#include "utils_timestamp.h"
#include "selinux_label.h"
#include "err_msg.h"

struct io_read_wrapper;

#define OVERLAY_LINK_DIR "l"
#define OVERLAY_LAYER_DIFF "diff"
#define OVERLAY_LAYER_MERGED "merged"
#define OVERLAY_LAYER_WORK "work"
#define OVERLAY_LAYER_LOWER "lower"
#define OVERLAY_LAYER_LINK "link"
#define OVERLAY_LAYER_EMPTY "empty"

#define OVERLAY_LAYER_MAX_DEPTH 128

#define QUOTA_SIZE_OPTION "overlay2.size"
#define QUOTA_BASESIZE_OPTIONS "overlay2.basesize"
// MAX_LAYER_ID_LENGTH represents the number of random characters which can be used to create the unique link identifer
// for every layer. If this value is too long then the page size limit for the mount command may be exceeded.
// The idLength should be selected such that following equation is true (512 is a buffer for label metadata).
// ((idLength + len(linkDir) + 1) * maxDepth) <= (pageSize - 512)
#define MAX_LAYER_ID_LENGTH 26

void free_driver_create_opts(struct driver_create_opts *opts)
{
    if (opts == NULL) {
        return;
    }
    free(opts->mount_label);
    opts->mount_label = NULL;

    free_json_map_string_string(opts->storage_opt);
    opts->storage_opt = NULL;

    free(opts);
}

void free_driver_mount_opts(struct driver_mount_opts *opts)
{
    if (opts == NULL) {
        return;
    }
    free(opts->mount_label);
    opts->mount_label = NULL;

    util_free_array_by_len(opts->options, opts->options_len);
    opts->options = NULL;

    free(opts);
}

static int overlay2_parse_options(struct graphdriver *driver, const char **options, size_t options_len)
{
    int ret = 0;
    size_t i = 0;
    char *dup = NULL;
    char *p = NULL;
    char *val = NULL;
    struct overlay_options *overlay_opts = NULL;

    overlay_opts = util_common_calloc_s(sizeof(struct overlay_options));
    if (overlay_opts == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }
    driver->overlay_opts = overlay_opts;

    for (i = 0; options != NULL && i < options_len; i++) {
        dup = util_strdup_s(options[i]);
        if (dup == NULL) {
            ERROR("Out of memory");
            ret = -1;
            goto out;
        }
        p = strchr(dup, '=');
        if (!p) {
            ERROR("Unable to parse key/value option: '%s'", dup);
            ret = -1;
            goto out;
        }
        *p = '\0';
        val = p + 1;
        if (strcasecmp(dup, QUOTA_SIZE_OPTION) == 0) {
            int64_t converted = 0;
            ret = util_parse_byte_size_string(val, &converted);
            if (ret != 0) {
                ERROR("Invalid size: '%s': %s", val, strerror(-ret));
                ret = -1;
                goto out;
            }
            overlay_opts->default_quota = converted;
        } else if (strcasecmp(dup, QUOTA_BASESIZE_OPTIONS) == 0) {
            int64_t converted = 0;
            ret = util_parse_byte_size_string(val, &converted);
            if (ret != 0) {
                ERROR("Invalid size: '%s': %s", val, strerror(-ret));
                ret = -1;
                goto out;
            }
            overlay_opts->default_quota = converted;
        } else if (strcasecmp(dup, "overlay2.override_kernel_check") == 0) {
            bool converted_bool = 0;
            ret = util_str_to_bool(val, &converted_bool);
            if (ret != 0) {
                ERROR("Invalid bool: '%s': %s", val, strerror(-ret));
                ret = -1;
                goto out;
            }
            overlay_opts->override_kernelcheck = converted_bool;
        } else if (strcasecmp(dup, "overlay2.skip_mount_home") == 0) {
            bool converted_bool = 0;
            ret = util_str_to_bool(val, &converted_bool);
            if (ret != 0) {
                ERROR("Invalid bool: '%s': %s", val, strerror(-ret));
                ret = -1;
                goto out;
            }
            overlay_opts->skip_mount_home = converted_bool;
        } else if (strcasecmp(dup, "overlay2.mountopt") == 0) {
            overlay_opts->mount_options = util_strdup_s(val);
        } else {
            ERROR("Overlay2: unknown option: '%s'", dup);
            ret = -1;
            goto out;
        }
        free(dup);
        dup = NULL;
    }

out:
    free(dup);
    return ret;
}

static bool check_bk_fs_support_overlay(const char *backing_fs)
{
    if (strcmp(backing_fs, "aufs") == 0 || strcmp(backing_fs, "zfs") == 0 || strcmp(backing_fs, "overlayfs") == 0 ||
        strcmp(backing_fs, "ecryptfs") == 0) {
        return false;
    }
    return true;
}

static void check_link_file_valid(const char *fname)
{
    int nret = 0;
    struct stat fstat;

    nret = stat(fname, &fstat);
    if (nret != 0) {
        if (errno == ENOENT) {
            WARN("[overlay2]: remove invalid symlink: %s", fname);
            if (util_path_remove(fname) != 0) {
                SYSERROR("Failed to remove link path %s", fname);
            }
        } else {
            SYSERROR("[overlay2]: Evaluate symlink %s failed", fname);
        }
    }
}

static void rm_invalid_symlink(const char *dirpath)
{
    int nret = 0;
    struct dirent *pdirent = NULL;
    DIR *directory = NULL;
    char fname[PATH_MAX] = { 0 };

    directory = opendir(dirpath);
    if (directory == NULL) {
        ERROR("Failed to open %s", dirpath);
        return;
    }
    pdirent = readdir(directory);
    for (; pdirent != NULL; pdirent = readdir(directory)) {
        int pathname_len;

        if (!strcmp(pdirent->d_name, ".") || !strcmp(pdirent->d_name, "..")) {
            continue;
        }

        (void)memset(fname, 0, sizeof(fname));

        pathname_len = snprintf(fname, PATH_MAX, "%s/%s", dirpath, pdirent->d_name);
        if (pathname_len < 0 || pathname_len >= PATH_MAX) {
            ERROR("Pathname too long");
            continue;
        }

        check_link_file_valid(fname);
    }

    nret = closedir(directory);
    if (nret) {
        ERROR("Failed to close directory %s", dirpath);
    }

    return;
}

static bool check_bk_fs_support_quota(const char *backing_fs)
{
    return strcmp(backing_fs, "xfs") == 0 || strcmp(backing_fs, "extfs") == 0;
}

static int driver_init_quota(struct graphdriver *driver)
{
    int ret = 0;

    if (check_bk_fs_support_quota(driver->backing_fs)) {
        driver->quota_ctrl = project_quota_control_init(driver->home, driver->backing_fs);
        if (driver->quota_ctrl != NULL) {
            driver->support_quota = true;
        } else if (driver->overlay_opts->default_quota != 0) {
            ERROR("Storage option overlay.size not supported. Filesystem does not support Project Quota");
            ret = -1;
            goto out;
        }
    } else if (driver->overlay_opts->default_quota != 0) {
        ERROR("Storage option overlay.size only supported for backingFS XFS or ext4.");
        ret = -1;
        goto out;
    }

out:
    return ret;
}

int overlay2_init(struct graphdriver *driver, const char *drvier_home, const char **options, size_t len)
{
    int ret = 0;
    char *p = NULL;
    char *link_dir = NULL;
    char *root_dir = NULL;

    if (driver == NULL || drvier_home == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    if (!util_support_overlay()) {
        ERROR("driver \'%s\'not supported", driver->name);
        ret = -1;
        goto out;
    }

    ret = overlay2_parse_options(driver, options, len);
    if (ret != 0) {
        ret = -1;
        goto out;
    }
    
    link_dir = util_path_join(drvier_home, OVERLAY_LINK_DIR);
    if (link_dir == NULL) {
        ERROR("Unable to create driver link directory %s.", drvier_home);
        ret = -1;
        goto out;
    }
    // 创建l目录
    if (util_mkdir_p(link_dir, 0700) != 0) {
        ERROR("Unable to create driver home directory %s.", link_dir);
        ret = -1;
        goto out;
    }
    // 考虑修改overlay/l两个目录的权限
    if (set_file_owner_for_user_remap(link_dir, conf_get_isulad_user_remap()) != 0) {
        ERROR("Unable to change directory %s owner for user remap.", link_dir);
        ret = -1;
        goto out;
    }

    // find parent directory
    p = strrchr(link_dir, '/');
    if (p == NULL) {
        ERROR("Failed to find parent directory for %s", link_dir);
        goto out;
    }
    *p = '\0';

    if (set_file_owner_for_user_remap(link_dir, conf_get_isulad_user_remap()) != 0) {
        ERROR("Unable to change directory %s owner for user remap.", link_dir);
        ret = -1;
        goto out;
    }
    
    rm_invalid_symlink(link_dir);

    driver->home = util_strdup_s(drvier_home);

    root_dir = util_path_dir(drvier_home);
    if (root_dir == NULL) {
        ERROR("Unable to get driver root home directory %s.", drvier_home);
        ret = -1;
        goto out;
    }

    driver->backing_fs = util_get_fs_name(root_dir);
    if (driver->backing_fs == NULL) {
        ERROR("Failed to get backing fs");
        ret = -1;
        goto out;
    }

    if (!check_bk_fs_support_overlay(driver->backing_fs)) {
        ERROR("'overlay' is not supported over backing file system %s", driver->backing_fs);
        ret = -1;
        goto out;
    }

    if (!util_support_d_type(drvier_home)) {
        ERROR("The backing %s filesystem is formatted without d_type support, which leads to incorrect behavior.",
              driver->backing_fs);
        ret = -1;
        goto out;
    }
    driver->support_dtype = true;

    if (!driver->overlay_opts->skip_mount_home) {
        if (util_ensure_mounted_as(drvier_home, "private") != 0) {
            ret = -1;
            goto out;
        }
    }

    if (driver_init_quota(driver) != 0) {
        ret = -1;
        goto out;
    }

out:
    free(link_dir);
    free(root_dir);
    return ret;
}

bool overlay2_is_quota_options(struct graphdriver *driver, const char *option)
{
    return strncmp(option, QUOTA_SIZE_OPTION, strlen(QUOTA_SIZE_OPTION)) == 0 ||
           strncmp(option, QUOTA_BASESIZE_OPTIONS, strlen(QUOTA_BASESIZE_OPTIONS)) == 0;
}

static int check_parent_valid(const char *parent, const struct graphdriver *driver)
{
    int ret = 0;
    char *parent_dir = NULL;

    if (parent != NULL) {
        parent_dir = util_path_join(driver->home, parent);
        if (parent_dir == NULL) {
            ERROR("Failed to join layer dir:%s", parent);
            ret = -1;
            goto out;
        }
        if (!util_dir_exists(parent_dir)) {
            SYSERROR("parent layer %s not exists", parent_dir);
            ret = -1;
            goto out;
        }
    }
out:
    free(parent_dir);
    return ret;
}

static int mk_diff_directory(const char *layer_dir)
{
    int ret = 0;
    char *diff_dir = NULL;

    diff_dir = util_path_join(layer_dir, OVERLAY_LAYER_DIFF);
    if (diff_dir == NULL) {
        ERROR("Failed to join layer diff dir:%s", layer_dir);
        ret = -1;
        goto out;
    }

    if (util_mkdir_p(diff_dir, DEFAULT_HIGHEST_DIRECTORY_MODE) != 0) {
        ERROR("Unable to create layer diff directory %s.", diff_dir);
        ret = -1;
        goto out;
    }

    if (set_file_owner_for_user_remap(diff_dir, conf_get_isulad_user_remap()) != 0) {
        ERROR("Unable to change directory %s owner for user remap.", diff_dir);
        ret = -1;
        goto out;
    }

out:
    free(diff_dir);
    return ret;
}

static int do_diff_symlink(const char *id, char *link_id, const char *driver_home)
{
    int ret = 0;
    int nret = 0;
    char target_path[PATH_MAX] = { 0 };
    char link_path[PATH_MAX] = { 0 };
    char clean_path[PATH_MAX] = { 0 };

    nret = snprintf(target_path, PATH_MAX, "../%s/diff", id);
    if (nret < 0 || nret >= PATH_MAX) {
        ERROR("Failed to get target path %s", id);
        ret = -1;
        goto out;
    }

    nret = snprintf(link_path, PATH_MAX, "%s/%s/%s", driver_home, OVERLAY_LINK_DIR, link_id);
    if (nret < 0 || nret >= PATH_MAX) {
        ERROR("Failed to get link path %s", link_id);
        ret = -1;
        goto out;
    }

    if (util_clean_path(link_path, clean_path, sizeof(clean_path)) == NULL) {
        ERROR("failed to get clean path %s", link_path);
        ret = -1;
        goto out;
    }

    nret = symlink(target_path, clean_path);
    if (ret < 0) {
        SYSERROR("Failed to create symlink from \"%s\" to \"%s\"", clean_path, target_path);
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int mk_diff_symlink(const char *id, const char *layer_dir, const char *driver_home)
{
    int ret = 0;
    char layer_id[MAX_LAYER_ID_LENGTH + 1] = { 0 };
    char *link_file = NULL;

    ret = util_generate_random_str(layer_id, MAX_LAYER_ID_LENGTH);
    if (ret != 0) {
        ERROR("Failed to get layer symlink id %s", id);
        ret = -1;
        goto out;
    }

    ret = do_diff_symlink(id, layer_id, driver_home);
    if (ret != 0) {
        ERROR("Failed to do symlink id %s", id);
        ret = -1;
        goto out;
    }

    link_file = util_path_join(layer_dir, OVERLAY_LAYER_LINK);
    if (link_file == NULL) {
        ERROR("Failed to get layer link file %s", layer_dir);
        ret = -1;
        goto out;
    }

    ret = util_atomic_write_file(link_file, layer_id, strlen(layer_id), 0644, false);
    if (ret) {
        SYSERROR("Failed to write %s", link_file);
        ret = -1;
        goto out;
    }

out:
    free(link_file);
    return ret;
}

static int mk_work_directory(const char *layer_dir)
{
    int ret = 0;
    char *work_dir = NULL;

    work_dir = util_path_join(layer_dir, OVERLAY_LAYER_WORK);
    if (work_dir == NULL) {
        ERROR("Failed to join layer work dir:%s", layer_dir);
        ret = -1;
        goto out;
    }

    if (util_mkdir_p(work_dir, 0700) != 0) {
        ERROR("Unable to create layer work directory %s.", work_dir);
        ret = -1;
        goto out;
    }

    if (set_file_owner_for_user_remap(work_dir, conf_get_isulad_user_remap()) != 0) {
        ERROR("Unable to change directory %s owner for user remap.", work_dir);
        ret = -1;
        goto out;
    }

out:
    free(work_dir);
    return ret;
}

static int mk_merged_directory(const char *layer_dir)
{
    int ret = 0;
    char *merged_dir = NULL;

    merged_dir = util_path_join(layer_dir, OVERLAY_LAYER_MERGED);
    if (merged_dir == NULL) {
        ERROR("Failed to join layer merged dir:%s", layer_dir);
        ret = -1;
        goto out;
    }

    if (util_mkdir_p(merged_dir, 0700) != 0) {
        ERROR("Unable to create layer merged directory %s.", merged_dir);
        ret = -1;
        goto out;
    }
    
    if (set_file_owner_for_user_remap(merged_dir, conf_get_isulad_user_remap()) != 0) {
        ERROR("Unable to change directory %s owner for user remap.", merged_dir);
        ret = -1;
        goto out;
    }
out:
    free(merged_dir);
    return ret;
}

static int mk_empty_directory(const char *layer_dir)
{
    int ret = 0;
    char *empty_dir = NULL;

    empty_dir = util_path_join(layer_dir, OVERLAY_LAYER_EMPTY);
    if (empty_dir == NULL) {
        ERROR("Failed to join layer empty dir:%s", layer_dir);
        ret = -1;
        goto out;
    }

    if (util_mkdir_p(empty_dir, 0700) != 0) {
        ERROR("Unable to create layer empty directory %s.", empty_dir);
        ret = -1;
        goto out;
    }

out:
    free(empty_dir);
    return ret;
}

const static int check_lower_depth(const char *lowers_str)
{
    int ret = 0;
    char **lowers_arr = NULL;
    size_t lowers_size = 0;

    lowers_arr = util_string_split(lowers_str, ':');
    lowers_size = util_array_len((const char **)lowers_arr);

    if (lowers_size > OVERLAY_LAYER_MAX_DEPTH) {
        ERROR("Max depth exceeded %s", lowers_str);
        ret = -1;
        goto out;
    }

out:
    util_free_array(lowers_arr);
    return ret;
}

static char *get_lower(const char *parent, const char *driver_home)
{
    int nret = 0;
    char *lower = NULL;
    size_t lower_len = 0;
    char *parent_dir = NULL;
    char *parent_link_file = NULL;
    char *parent_link = NULL;
    char *parent_lower_file = NULL;
    char *parent_lowers = NULL;

    parent_dir = util_path_join(driver_home, parent);
    if (parent_dir == NULL) {
        ERROR("Failed to get parent dir %s", parent);
        goto out;
    }

    parent_link_file = util_path_join(parent_dir, OVERLAY_LAYER_LINK);
    if (parent_link_file == NULL) {
        ERROR("Failed to get parent link %s", parent_dir);
        goto out;
    }

    parent_link = util_read_text_file(parent_link_file);
    if (parent_link == NULL) {
        ERROR("Failed to read parent link %s", parent_link_file);
        goto out;
    }

    if (strlen(parent_link) >= (INT_MAX - strlen(OVERLAY_LINK_DIR) - 2)) {
        ERROR("parent link %s too large", parent_link_file);
        goto out;
    }

    lower_len = strlen(OVERLAY_LINK_DIR) + 1 + strlen(parent_link) + 1;

    parent_lower_file = util_path_join(parent_dir, OVERLAY_LAYER_LOWER);
    if (parent_lower_file == NULL) {
        ERROR("Failed to get parent lower %s", parent_dir);
        goto out;
    }

    parent_lowers = util_read_text_file(parent_lower_file);
    if (parent_lowers != NULL) {
        if (strlen(parent_lowers) >= (INT_MAX - lower_len - 1)) {
            ERROR("parent lower %s too large", parent_link_file);
            goto out;
        }
        lower_len = lower_len + strlen(parent_lowers) + 1;
    }

    lower = util_common_calloc_s(lower_len);
    if (lower == NULL) {
        ERROR("Memory out");
        goto err_out;
    }

    if (parent_lowers != NULL) {
        nret = snprintf(lower, lower_len, "%s/%s:%s", OVERLAY_LINK_DIR, parent_link, parent_lowers);
    } else {
        nret = snprintf(lower, lower_len, "%s/%s", OVERLAY_LINK_DIR, parent_link);
    }
    if (nret < 0 || nret >= lower_len) {
        ERROR("lower %s too large", parent_link);
        goto err_out;
    }

    if (check_lower_depth(lower) != 0) {
        goto err_out;
    }

    goto out;

err_out:
    free(lower);
    lower = NULL;

out:
    free(parent_dir);
    free(parent_link_file);
    free(parent_link);
    free(parent_lower_file);
    free(parent_lowers);
    return lower;
}

static int write_lowers(const char *layer_dir, const char *lowers)
{
    int ret = 0;
    char *lowers_file = NULL;

    lowers_file = util_path_join(layer_dir, OVERLAY_LAYER_LOWER);
    if (lowers_file == NULL) {
        ERROR("Failed to get layer lower file %s", layer_dir);
        ret = -1;
        goto out;
    }

    ret = util_atomic_write_file(lowers_file, lowers, strlen(lowers), 0666, false);
    if (ret) {
        SYSERROR("Failed to write %s", lowers_file);
        ret = -1;
        goto out;
    }

out:
    free(lowers_file);
    return ret;
}

static int mk_sub_directories(const char *id, const char *parent, const char *layer_dir, const char *driver_home)
{
    int ret = 0;
    char *lowers = NULL;

    if (mk_diff_directory(layer_dir) != 0) {
        ret = -1;
        goto out;
    }

    if (mk_diff_symlink(id, layer_dir, driver_home) != 0) {
        ret = -1;
        goto out;
    }

    if (mk_work_directory(layer_dir) != 0) {
        ret = -1;
        goto out;
    }

    if (mk_merged_directory(layer_dir) != 0) {
        ret = -1;
        goto out;
    }

    if (parent == NULL) {
        if (mk_empty_directory(layer_dir) != 0) {
            ret = -1;
            goto out;
        }
    } else {
        lowers = get_lower(parent, driver_home);
        if (lowers == NULL) {
            ret = -1;
            goto out;
        }
        if (write_lowers(layer_dir, lowers) != 0) {
            ret = -1;
            goto out;
        }
    }

out:
    free(lowers);
    return ret;
}

static int set_layer_quota(const char *dir, const json_map_string_string *opts, const struct graphdriver *driver)
{
    int ret = 0;
    size_t i = 0;
    uint64_t quota = 0;

    for (i = 0; i < opts->len; i++) {
        if (strcasecmp("size", opts->keys[i]) == 0) {
            int64_t converted = 0;
            ret = util_parse_byte_size_string(opts->values[i], &converted);
            if (ret != 0) {
                ERROR("Invalid size: '%s': %s", opts->values[i], strerror(-ret));
                isulad_set_error_message("Invalid quota size: '%s': %s", opts->values[i], strerror(-ret));
                ret = -1;
                goto out;
            }
            quota = (uint64_t)converted;
            break;
        } else {
            ERROR("Unknown option %s", opts->keys[i]);
            isulad_set_error_message("Unknown storage option %s", opts->keys[i]);
            ret = -1;
            goto out;
        }
    }

    if (quota > 0 && quota < 4096) {
        ERROR("Illegal storage quota size %lu, 4096 at least", quota);
        isulad_set_error_message("Illegal storage quota size %lu, 4096 at least", quota);
        ret = -1;
        goto out;
    }

    if (quota == 0) {
        quota = driver->overlay_opts->default_quota;
    }

    if (quota > 0) {
        ret = driver->quota_ctrl->set_quota(dir, driver->quota_ctrl, quota);
    }

out:
    return ret;
}

static int do_create(const char *id, const char *parent, const struct graphdriver *driver,
                     const struct driver_create_opts *create_opts)
{
    int ret = 0;
    char *layer_dir = NULL;

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        ret = -1;
        goto out;
    }

    if (check_parent_valid(parent, driver) != 0) {
        ret = -1;
        goto out;
    }

    if (util_mkdir_p(layer_dir, 0700) != 0) {
        ERROR("Unable to create layer directory %s.", layer_dir);
        ret = -1;
        goto out;
    }
    
    if (set_file_owner_for_user_remap(layer_dir, conf_get_isulad_user_remap()) != 0) {
        ERROR("Unable to change directory %s owner for user remap.", layer_dir);
        ret = -1;
        goto out;
    }

    if (create_opts->storage_opt != NULL && create_opts->storage_opt->len != 0) {
        if (set_layer_quota(layer_dir, create_opts->storage_opt, driver) != 0) {
            ERROR("Unable to set layer quota %s", layer_dir);
            ret = -1;
            goto err_out;
        }
    }

    if (mk_sub_directories(id, parent, layer_dir, driver->home) != 0) {
        ret = -1;
        goto err_out;
    }

    goto out;

err_out:
    if (util_recursive_rmdir(layer_dir, 0)) {
        ERROR("Failed to delete layer path: %s", layer_dir);
    }

out:
    free(layer_dir);
    return ret;
}

static int append_default_quota_opts(struct driver_create_opts *ori_opts, uint64_t quota)
{
    int ret = 0;
    int nret = 0;
    size_t i = 0;
    char tmp[50] = { 0 }; //tmp to hold unit64

    if (quota == 0) {
        return 0;
    }

    nret = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)quota);
    if (nret < 0 || (size_t)nret >= sizeof(tmp)) {
        ERROR("Failed to make quota string");
        ret = -1;
        goto out;
    }

    if (ori_opts->storage_opt == NULL) {
        ori_opts->storage_opt = util_common_calloc_s(sizeof(json_map_string_string));
        if (ori_opts->storage_opt == NULL) {
            ERROR("Memory out");
            ret = -1;
            goto out;
        }
    }

    for (i = 0; i < ori_opts->storage_opt->len; i++) {
        if (strcasecmp("size", ori_opts->storage_opt->keys[i]) == 0) {
            break;
        }
    }
    if (i == ori_opts->storage_opt->len) {
        ret = append_json_map_string_string(ori_opts->storage_opt, "size", tmp);
        if (ret != 0) {
            ERROR("Failed to append quota size option");
            ret = -1;
            goto out;
        }
    }

out:
    return ret;
}

int overlay2_create_rw(const char *id, const char *parent, const struct graphdriver *driver,
                       struct driver_create_opts *create_opts)
{
    int ret = 0;

    if (id == NULL || driver == NULL || create_opts == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    if (create_opts->storage_opt != NULL && create_opts->storage_opt->len != 0 && !driver->support_quota) {
        ERROR("--storage-opt is supported only for overlay over xfs or ext4 with 'pquota' mount option");
        isulad_set_error_message(
            "--storage-opt is supported only for overlay over xfs or ext4 with 'pquota' mount option");
        ret = -1;
        goto out;
    }

    if (driver->support_quota && append_default_quota_opts(create_opts, driver->overlay_opts->default_quota) != 0) {
        ret = -1;
        goto out;
    }

    ret = do_create(id, parent, driver, create_opts);

out:
    return ret;
}

int overlay2_create_ro(const char *id, const char *parent, const struct graphdriver *driver,
                       const struct driver_create_opts *create_opts)
{
    if (id == NULL || driver == NULL || create_opts == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    if (create_opts->storage_opt != NULL && create_opts->storage_opt->len != 0) {
        ERROR("--storage-opt size is only supported for ReadWrite Layers");
        return -1;
    }

    return do_create(id, parent, driver, create_opts);
}

static char *read_layer_link_file(const char *layer_dir)
{
    char *link_file = NULL;
    char *link = NULL;

    link_file = util_path_join(layer_dir, OVERLAY_LAYER_LINK);
    if (link_file == NULL) {
        ERROR("Failed to get link %s", layer_dir);
        goto out;
    }

    link = util_read_text_file(link_file);
out:
    free(link_file);
    return link;
}

static char *read_layer_lower_file(const char *layer_dir)
{
    char *lower_file = NULL;
    char *lower = NULL;

    lower_file = util_path_join(layer_dir, OVERLAY_LAYER_LOWER);
    if (lower_file == NULL) {
        ERROR("Failed to get lower %s", layer_dir);
        goto out;
    }

    lower = util_read_text_file(lower_file);
out:
    free(lower_file);
    return lower;
}

int overlay2_rm_layer(const char *id, const struct graphdriver *driver)
{
    int ret = 0;
    int nret = 0;
    char *layer_dir = NULL;
    char *link_id = NULL;
    char link_path[PATH_MAX] = { 0 };
    char clean_path[PATH_MAX] = { 0 };

    if (id == NULL || driver == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        ret = -1;
        goto out;
    }

    link_id = read_layer_link_file(layer_dir);
    if (link_id != NULL) {
        nret = snprintf(link_path, PATH_MAX, "%s/%s/%s", driver->home, OVERLAY_LINK_DIR, link_id);
        if (nret < 0 || nret >= PATH_MAX) {
            ERROR("Failed to get link path %s", link_id);
            ret = -1;
            goto out;
        }
        if (util_clean_path(link_path, clean_path, sizeof(clean_path)) == NULL) {
            ERROR("failed to get clean path %s", link_path);
            ret = -1;
            goto out;
        }
        // ignore error
        if (util_path_remove(clean_path) != 0) {
            SYSERROR("Failed to remove link path %s", clean_path);
        }
    }

    if (util_recursive_rmdir(layer_dir, 0) != 0) {
        SYSERROR("Failed to remove layer directory %s", layer_dir);
        ret = -1;
        goto out;
    }

out:
    free(layer_dir);
    free(link_id);
    return ret;
}

static int append_abs_lower_path(const char *driver_home, const char *lower, char ***abs_lowers)
{
    int ret = 0;
    char *abs_path = NULL;

    abs_path = util_path_join(driver_home, lower);
    if (!util_dir_exists(abs_path)) {
        SYSERROR("Can't stat absolute layer:%s", abs_path);
        ret = -1;
        goto out;
    }
    if (util_array_append(abs_lowers, abs_path) != 0) {
        SYSERROR("Can't append absolute layer:%s", abs_path);
        ret = -1;
        goto out;
    }

out:
    free(abs_path);
    return ret;
}

static int append_abs_empty_path(const char *layer_dir, char ***abs_lowers)
{
    int ret = 0;
    char *abs_path = NULL;

    abs_path = util_path_join(layer_dir, OVERLAY_LAYER_EMPTY);
    if (!util_dir_exists(abs_path)) {
        SYSERROR("Can't stat absolute layer:%s", abs_path);
        ret = -1;
        goto out;
    }
    if (util_array_append(abs_lowers, abs_path) != 0) {
        SYSERROR("Can't append absolute layer:%s", abs_path);
        ret = -1;
        goto out;
    }

out:
    free(abs_path);
    return ret;
}

static int append_rel_empty_path(const char *id, char ***rel_lowers)
{
    int ret = 0;
    char *rel_path = NULL;

    rel_path = util_string_append("/empty", id);

    if (util_array_append(rel_lowers, rel_path) != 0) {
        SYSERROR("Can't append relative layer:%s", rel_path);
        ret = -1;
        goto out;
    }

out:
    free(rel_path);
    return ret;
}

static int get_mount_opt_lower_dir(const char *id, const char *layer_dir, const char *driver_home, char **abs_lower_dir,
                                   char **rel_lower_dir)
{
    int ret = 0;
    char *lowers_str = NULL;
    char **lowers = NULL;
    char **abs_lowers = NULL;
    char **rel_lowers = NULL;
    size_t lowers_size = 0;
    size_t i = 0;

    lowers_str = read_layer_lower_file(layer_dir);
    lowers = util_string_split(lowers_str, ':');
    lowers_size = util_array_len((const char **)lowers);

    for (i = 0; i < lowers_size; i++) {
        if (append_abs_lower_path(driver_home, lowers[i], &abs_lowers) != 0) {
            ret = -1;
            goto out;
        }

        if (util_array_append(&rel_lowers, lowers[i]) != 0) {
            SYSERROR("Can't append relative layer:%s", lowers[i]);
            ret = -1;
            goto out;
        }
    }

    // If the lowers list is still empty, use an empty lower
    if (util_array_len((const char **)abs_lowers) == 0) {
        if (append_abs_empty_path(layer_dir, &abs_lowers) != 0) {
            ret = -1;
            goto out;
        }
        if (append_rel_empty_path(id, &rel_lowers) != 0) {
            ret = -1;
            goto out;
        }
    }

    *abs_lower_dir = util_string_join(":", (const char **)abs_lowers, util_array_len((const char **)abs_lowers));
    *rel_lower_dir = util_string_join(":", (const char **)rel_lowers, util_array_len((const char **)rel_lowers));
    if ((*abs_lower_dir) == NULL || (*rel_lower_dir) == NULL) {
        ERROR("memory out");
        free(*abs_lower_dir);
        *abs_lower_dir = NULL;
        free(*rel_lower_dir);
        *rel_lower_dir = NULL;
        ret = -1;
        goto out;
    }

out:
    free(lowers_str);
    util_free_array(lowers);
    util_free_array(abs_lowers);
    util_free_array(rel_lowers);

    return ret;
}

static char *get_mount_opt_data_with_custom_option(size_t cur_size, const char *cur_opts,
                                                   const struct driver_mount_opts *mount_opts)
{
    int nret = 0;
    char *mount_data = NULL;
    char *custom_opts = NULL;
    size_t data_size = 0;

    custom_opts = util_string_join(",", (const char **)(mount_opts->options), mount_opts->options_len);
    if (custom_opts == NULL) {
        ERROR("Failed to get custom mount opts");
        goto error_out;
    }

    if (strlen(custom_opts) >= (INT_MAX - cur_size - 1)) {
        ERROR("custom mount option too large");
        goto error_out;
    }

    data_size = cur_size + strlen(custom_opts) + 1;
    mount_data = util_common_calloc_s(data_size);
    if (mount_data == NULL) {
        ERROR("Memory out");
        goto error_out;
    }

    nret = snprintf(mount_data, data_size, "%s,%s", custom_opts, cur_opts);
    if (nret < 0 || (size_t)nret >= data_size) {
        ERROR("Failed to get custom opts data");
        goto error_out;
    }

    goto out;

error_out:
    free(mount_data);
    mount_data = NULL;

out:
    free(custom_opts);
    return mount_data;
}

static char *get_mount_opt_data_with_driver_option(size_t cur_size, const char *cur_opts, const char *mount_opts)
{
    int nret = 0;
    char *mount_data = NULL;
    size_t data_size = 0;

    if (strlen(mount_opts) >= (INT_MAX - cur_size - 1)) {
        ERROR("driver mount option too large");
        goto error_out;
    }

    data_size = cur_size + strlen(mount_opts) + 1;
    mount_data = util_common_calloc_s(data_size);
    if (mount_data == NULL) {
        ERROR("Memory out");
        goto error_out;
    }

    nret = snprintf(mount_data, data_size, "%s,%s", mount_opts, cur_opts);
    if (nret < 0 || (size_t)nret >= data_size) {
        ERROR("Failed to get driver opts data");
        goto error_out;
    }

    goto out;

error_out:
    free(mount_data);
    mount_data = NULL;

out:
    return mount_data;
}

static char *get_abs_mount_opt_data(const char *layer_dir, const char *abs_lower_dir, const struct graphdriver *driver,
                                    const struct driver_mount_opts *mount_opts)
{
    int nret = 0;
    char *mount_data = NULL;
    size_t data_size = 0;
    char *upper_dir = NULL;
    char *work_dir = NULL;
    char *tmp = NULL;

    upper_dir = util_path_join(layer_dir, OVERLAY_LAYER_DIFF);
    if (upper_dir == NULL) {
        ERROR("Failed to join layer diff dir:%s", layer_dir);
        goto error_out;
    }

    work_dir = util_path_join(layer_dir, OVERLAY_LAYER_WORK);
    if (work_dir == NULL) {
        ERROR("Failed to join layer work dir:%s", layer_dir);
        goto error_out;
    }

    if (strlen(abs_lower_dir) >= (INT_MAX - strlen("lowerdir=") - strlen(",upperdir=") - strlen(upper_dir) -
                                  strlen(",workdir=") - strlen(work_dir) - 1)) {
        ERROR("abs lower dir too large");
        goto error_out;
    }
    data_size = strlen("lowerdir=") + strlen(abs_lower_dir) + strlen(",upperdir=") + strlen(upper_dir) +
                strlen(",workdir=") + strlen(work_dir) + 1;

    mount_data = util_common_calloc_s(data_size);
    if (mount_data == NULL) {
        ERROR("Memory out");
        goto error_out;
    }

    nret = snprintf(mount_data, data_size, "lowerdir=%s,upperdir=%s,workdir=%s", abs_lower_dir, upper_dir, work_dir);
    if (nret < 0 || (size_t)nret >= data_size) {
        ERROR("abs lower dir too large");
        goto error_out;
    }

    if (mount_opts != NULL && mount_opts->options_len != 0) {
        tmp = get_mount_opt_data_with_custom_option(data_size, mount_data, mount_opts);
        if (tmp == NULL) {
            goto error_out;
        }
        free(mount_data);
        mount_data = tmp;
        tmp = NULL;
    } else if (driver->overlay_opts->mount_options != NULL) {
        tmp = get_mount_opt_data_with_driver_option(data_size, mount_data, driver->overlay_opts->mount_options);
        if (tmp == NULL) {
            goto error_out;
        }
        free(mount_data);
        mount_data = tmp;
        tmp = NULL;
    }

#ifdef ENABLE_SELINUX
    if (mount_opts != NULL && mount_opts->mount_label != NULL) {
        tmp = selinux_format_mountlabel(mount_data, mount_opts->mount_label);
        if (tmp == NULL) {
            goto error_out;
        }
        free(mount_data);
        mount_data = tmp;
        tmp = NULL;
    }
#endif

    goto out;

error_out:
    free(mount_data);
    mount_data = NULL;

out:
    free(upper_dir);
    free(work_dir);
    return mount_data;
}

static char *get_rel_mount_opt_data(const char *id, const char *rel_lower_dir, const struct graphdriver *driver,
                                    const struct driver_mount_opts *mount_opts)
{
    int nret = 0;
    char *mount_data = NULL;
    size_t data_size = 0;
    char *upper_dir = NULL;
    char *work_dir = NULL;
    char *tmp = NULL;

    upper_dir = util_string_append("/diff", id);
    if (upper_dir == NULL) {
        ERROR("Failed to join layer diff dir:%s", id);
        goto error_out;
    }

    work_dir = util_string_append("/work", id);
    if (work_dir == NULL) {
        ERROR("Failed to join layer work dir:%s", id);
        goto error_out;
    }

    if (strlen(rel_lower_dir) >= (INT_MAX - strlen("lowerdir=") - strlen(",upperdir=") - strlen(upper_dir) -
                                  strlen(",workdir=") - strlen(work_dir) - 1)) {
        ERROR("rel lower dir too large");
        goto error_out;
    }
    data_size = strlen("lowerdir=") + strlen(rel_lower_dir) + strlen(",upperdir=") + strlen(upper_dir) +
                strlen(",workdir=") + strlen(work_dir) + 1;

    mount_data = util_common_calloc_s(data_size);
    if (mount_data == NULL) {
        ERROR("Memory out");
        goto error_out;
    }

    nret = snprintf(mount_data, data_size, "lowerdir=%s,upperdir=%s,workdir=%s", rel_lower_dir, upper_dir, work_dir);
    if (nret < 0 || (size_t)nret >= data_size) {
        ERROR("rel lower dir too large");
        goto error_out;
    }

    if (mount_opts != NULL && mount_opts->options_len != 0) {
        tmp = get_mount_opt_data_with_custom_option(data_size, mount_data, mount_opts);
        if (tmp == NULL) {
            goto error_out;
        }
        free(mount_data);
        mount_data = tmp;
        tmp = NULL;
    } else if (driver->overlay_opts->mount_options != NULL) {
        tmp = get_mount_opt_data_with_driver_option(data_size, mount_data, driver->overlay_opts->mount_options);
        if (tmp == NULL) {
            goto error_out;
        }
        free(mount_data);
        mount_data = tmp;
        tmp = NULL;
    }

#ifdef ENABLE_SELINUX
    if (mount_opts != NULL && mount_opts->mount_label != NULL) {
        tmp = selinux_format_mountlabel(mount_data, mount_opts->mount_label);
        if (tmp == NULL) {
            goto error_out;
        }
        free(mount_data);
        mount_data = tmp;
        tmp = NULL;
    }
#endif

    goto out;

error_out:
    free(mount_data);
    mount_data = NULL;

out:
    free(upper_dir);
    free(work_dir);
    return mount_data;
}

static char *generate_mount_opt_data(const char *id, const char *layer_dir, const struct graphdriver *driver,
                                     const struct driver_mount_opts *mount_opts, bool *use_rel_mount)
{
    int ret = 0;
    char *mount_data = NULL;
    char *abs_lower_dir = NULL;
    char *rel_lower_dir = NULL;
    int page_size = getpagesize();

    ret = get_mount_opt_lower_dir(id, layer_dir, driver->home, &abs_lower_dir, &rel_lower_dir);
    if (ret != 0) {
        ERROR("Failed to get mount opt lower dir");
        goto out;
    }

    mount_data = get_abs_mount_opt_data(layer_dir, abs_lower_dir, driver, mount_opts);
    if (mount_data == NULL) {
        ERROR("Failed to get abs mount opt data");
        goto out;
    }
    if (strlen(mount_data) > page_size) {
        free(mount_data);
        *use_rel_mount = true;
        mount_data = get_rel_mount_opt_data(id, rel_lower_dir, driver, mount_opts);
        if (mount_data == NULL) {
            ERROR("Failed to get abs mount opt data");
            goto out;
        }
        if (strlen(mount_data) > page_size) {
            ERROR("cannot mount layer, mount label too large %s", mount_data);
            free(mount_data);
            mount_data = NULL;
            goto out;
        }
    }

out:
    free(abs_lower_dir);
    free(rel_lower_dir);
    return mount_data;
}

static int abs_mount(const char *layer_dir, const char *merged_dir, const char *mount_data)
{
    int ret = 0;

    ret = util_mount("overlay", merged_dir, "overlay", mount_data);
    if (ret != 0) {
        ERROR("Failed to mount %s with option \"%s\"", merged_dir, mount_data);
        goto out;
    }

out:
    return ret;
}

static int rel_mount(const char *driver_home, const char *id, const char *mount_data)
{
    int ret = 0;
    char *mount_target = NULL;

    mount_target = util_string_append("/merged", id);
    if (mount_target == NULL) {
        ERROR("Failed to join layer merged dir:%s", id);
        ret = -1;
        goto out;
    }

    ret = util_mount_from(driver_home, "overlay", mount_target, "overlay", mount_data);
    if (ret != 0) {
        ERROR("Failed to mount %s with option \"%s\"", mount_target, mount_data);
        ret = -1;
        goto out;
    }

out:
    free(mount_target);
    return ret;
}

static char *do_mount_layer(const char *id, const char *layer_dir, const struct graphdriver *driver,
                            const struct driver_mount_opts *mount_opts)
{
    char *merged_dir = NULL;
    char *mount_data = NULL;
    bool use_rel_mount = false;

    mount_data = generate_mount_opt_data(id, layer_dir, driver, mount_opts, &use_rel_mount);
    if (mount_data == NULL) {
        ERROR("Failed to get mount data");
        goto error_out;
    }

    merged_dir = util_path_join(layer_dir, OVERLAY_LAYER_MERGED);
    if (merged_dir == NULL) {
        ERROR("Failed to join layer merged dir:%s", layer_dir);
        goto error_out;
    }

    if (!use_rel_mount) {
        if (abs_mount(layer_dir, merged_dir, mount_data) != 0) {
            ERROR("Failed to mount %s with option \"%s\"", merged_dir, mount_data);
            goto error_out;
        }
    } else {
        if (rel_mount(driver->home, id, mount_data) != 0) {
            ERROR("Failed to mount %s from %s with option \"%s\"", id, driver->home, mount_data);
            goto error_out;
        }
    }

    goto out;

error_out:
    free(merged_dir);
    merged_dir = NULL;

out:
    free(mount_data);
    return merged_dir;
}

char *overlay2_mount_layer(const char *id, const struct graphdriver *driver, const struct driver_mount_opts *mount_opts)
{
    char *merged_dir = NULL;
    char *layer_dir = NULL;

    if (id == NULL || driver == NULL) {
        ERROR("Invalid input arguments");
        return NULL;
    }

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        goto out;
    }

    if (!util_dir_exists(layer_dir)) {
        SYSERROR("layer dir %s not exist", layer_dir);
        goto out;
    }

    merged_dir = do_mount_layer(id, layer_dir, driver, mount_opts);
    if (merged_dir == NULL) {
        ERROR("Failed to mount layer %s", id);
        goto out;
    }

out:
    free(layer_dir);
    return merged_dir;
}

int overlay2_umount_layer(const char *id, const struct graphdriver *driver)
{
    int ret = 0;
    char *merged_dir = NULL;
    char *layer_dir = NULL;

    if (id == NULL || driver == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        ret = -1;
        goto out;
    }

    // ignore error of umount point do not exist
    if (!util_dir_exists(layer_dir)) {
        SYSWARN("layer dir %s not exist", layer_dir);
        goto out;
    }

    merged_dir = util_path_join(layer_dir, OVERLAY_LAYER_MERGED);
    if (merged_dir == NULL) {
        ERROR("Failed to join layer merged dir:%s", layer_dir);
        ret = -1;
        goto out;
    }

    if (umount2(merged_dir, MNT_DETACH) && errno != EINVAL) {
        SYSERROR("Failed to umount the target: %s", merged_dir);
    }

out:
    free(layer_dir);
    free(merged_dir);
    return ret;
}

bool is_valid_layer_link(const char *link_id, const struct graphdriver *driver)
{
    bool valid = false;
    char *link_dir = NULL;
    char *link_file = NULL;
    struct stat fstat;

    link_dir = util_path_join(driver->home, OVERLAY_LINK_DIR);
    if (link_dir == NULL) {
        ERROR("Failed to join layer link dir:%s", driver->home);
        valid = false;
        goto out;
    }

    if (!util_dir_exists(link_dir)) {
        SYSERROR("link dir %s not exist", link_dir);
        valid = false;
        goto out;
    }

    link_file = util_path_join(link_dir, link_id);
    if (link_file == NULL) {
        ERROR("Failed to join layer link file:%s", link_id);
        valid = false;
        goto out;
    }

    if (stat(link_file, &fstat) != 0) {
        SYSERROR("[overlay2]: Check symlink %s failed, try to remove it", link_file);
        if (util_path_remove(link_file) != 0) {
            SYSERROR("Failed to remove link path %s", link_file);
        }
        valid = false;
        goto out;
    }

    valid = true;

out:
    free(link_dir);
    free(link_file);
    return valid;
}

bool overlay2_layer_exists(const char *id, const struct graphdriver *driver)
{
    int nret = 0;
    bool exists = false;
    char *layer_dir = NULL;
    char *link_id = NULL;

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        exists = false;
        goto out;
    }

    if (!util_dir_exists(layer_dir)) {
        SYSERROR("layer dir %s not exist", layer_dir);
        exists = false;
        goto out;
    }

    link_id = read_layer_link_file(layer_dir);
    if (link_id == NULL) {
        ERROR("Failed to get layer link data:%s", layer_dir);
        exists = false;
        goto out;
    }

    if (!is_valid_layer_link(link_id, driver)) {
        nret = do_diff_symlink(id, link_id, driver->home);
        if (nret != 0) {
            ERROR("Failed to do symlink id %s", id);
            exists = false;
            goto out;
        }
    }

    exists = true;

out:
    free(layer_dir);
    free(link_id);
    return exists;
}

int overlay2_apply_diff(const char *id, const struct graphdriver *driver, const struct io_read_wrapper *content)
{
    int ret = 0;

    unsigned int size = 0;   
    const char *user_remap = NULL;
    
    char *layer_dir = NULL;
    char *layer_diff = NULL;
    struct archive_options options = { 0 };
    char *err = NULL;

    if (id == NULL || driver == NULL || content == NULL) {
        ERROR("invalid argument");
        ret = -1;
        goto out;
    }

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        ret = -1;
        goto out;
    }

    layer_diff = util_path_join(layer_dir, OVERLAY_LAYER_DIFF);
    if (layer_diff == NULL) {
        ERROR("Failed to join layer diff dir:%s", id);
        ret = -1;
        goto out;
    }

    options.whiteout_format = OVERLAY_WHITEOUT_FORMATE;

    user_remap = conf_get_isulad_user_remap();
    if(user_remap != NULL){
        if (util_parse_user_remap(user_remap, &options.uid, &options.gid, &size)) {
            ERROR("Failed to split string '%s'.", user_remap);
            ret = -1;
            goto out;
        }
    }
    
    ret = archive_unpack(content, layer_diff, &options, &err);
    if (ret != 0) {
        ERROR("Failed to unpack to %s: %s", layer_diff, err);
        ret = -1;
        goto out;
    }

out:
    free(err);
    free(layer_dir);
    free(layer_diff);
    return ret;
}

static int get_lower_dirs(const char *layer_dir, const struct graphdriver *driver, char **abs_lower_dir)
{
    int ret = 0;
    char *lowers_str = NULL;
    char **lowers = NULL;
    char **abs_lowers = NULL;
    size_t lowers_size = 0;
    size_t i = 0;

    lowers_str = read_layer_lower_file(layer_dir);
    lowers = util_string_split(lowers_str, ':');
    lowers_size = util_array_len((const char **)lowers);

    if (lowers_size == 0) {
        ret = 0;
        goto out;
    }

    for (i = 0; i < lowers_size; i++) {
        if (append_abs_lower_path(driver->home, lowers[i], &abs_lowers) != 0) {
            ret = -1;
            goto out;
        }
    }

    *abs_lower_dir = util_string_join(":", (const char **)abs_lowers, util_array_len((const char **)abs_lowers));
    if (*abs_lower_dir == NULL) {
        ret = -1;
        goto out;
    }

out:
    free(lowers_str);
    util_free_array(lowers);
    util_free_array(abs_lowers);
    return ret;
}

int overlay2_get_layer_metadata(const char *id, const struct graphdriver *driver, json_map_string_string *map_info)
{
    int ret = 0;
    char *layer_dir = NULL;
    char *work_dir = NULL;
    char *merged_dir = NULL;
    char *upper_dir = NULL;
    char *lower_dir = NULL;

    if (id == NULL || driver == NULL || map_info == NULL) {
        ERROR("invalid argument");
        ret = -1;
        goto out;
    }

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        ret = -1;
        goto out;
    }

    work_dir = util_path_join(layer_dir, OVERLAY_LAYER_WORK);
    if (work_dir == NULL) {
        ERROR("Failed to join layer work dir:%s", layer_dir);
        ret = -1;
        goto out;
    }
    if (append_json_map_string_string(map_info, "WorkDir", work_dir) != 0) {
        ERROR("Failed to append layer work dir:%s", work_dir);
        ret = -1;
        goto out;
    }

    merged_dir = util_path_join(layer_dir, OVERLAY_LAYER_MERGED);
    if (merged_dir == NULL) {
        ERROR("Failed to join layer merged dir:%s", layer_dir);
        ret = -1;
        goto out;
    }
    if (append_json_map_string_string(map_info, "MergedDir", merged_dir) != 0) {
        ERROR("Failed to append layer merged dir:%s", merged_dir);
        ret = -1;
        goto out;
    }

    upper_dir = util_path_join(layer_dir, OVERLAY_LAYER_DIFF);
    if (upper_dir == NULL) {
        ERROR("Failed to join layer upper_dir dir:%s", layer_dir);
        ret = -1;
        goto out;
    }
    if (append_json_map_string_string(map_info, "UpperDir", upper_dir) != 0) {
        ERROR("Failed to append layer upper dir:%s", upper_dir);
        ret = -1;
        goto out;
    }

    if (get_lower_dirs(layer_dir, driver, &lower_dir) != 0) {
        ERROR("Failed to get layer lower dir:%s", layer_dir);
        ret = -1;
        goto out;
    }
    if (lower_dir != NULL && append_json_map_string_string(map_info, "LowerDir", lower_dir) != 0) {
        ERROR("Failed to append layer lower dir:%s", lower_dir);
        ret = -1;
        goto out;
    }

out:
    free(layer_dir);
    free(work_dir);
    free(merged_dir);
    free(upper_dir);
    free(lower_dir);
    return ret;
}

int overlay2_get_driver_status(const struct graphdriver *driver, struct graphdriver_status *status)
{
#define MAX_INFO_LENGTH 100
#define BACK_FS "Backing Filesystem"
#define SUPPORT_DTYPE "Supports d_type: true\n"
    int ret = 0;
    int nret = 0;
    char tmp[MAX_INFO_LENGTH] = { 0 };

    if (driver == NULL || status == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    status->driver_name = util_strdup_s(driver->name);

    status->backing_fs = util_strdup_s(driver->backing_fs);

    nret = snprintf(tmp, MAX_INFO_LENGTH, "%s: %s\n", BACK_FS, driver->backing_fs);
    if (nret < 0 || nret >= MAX_INFO_LENGTH) {
        ERROR("Failed to get backing fs");
        ret = -1;
        goto out;
    }

    status->status = util_string_append(SUPPORT_DTYPE, tmp);
    if (status->status == NULL) {
        ERROR("Failed to append SUPPORT_DTYPE");
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static void free_overlay_options(struct overlay_options *opts)
{
    if (opts == NULL) {
        return;
    }
    free((char *)opts->mount_options);
    opts->mount_options = NULL;

    free(opts);
}

int overlay2_clean_up(struct graphdriver *driver)
{
    int ret = 0;

    if (driver == NULL) {
        ERROR("Invalid input arguments");
        ret = -1;
        goto out;
    }

    if (umount(driver->home) != 0) {
        ret = -1;
        goto out;
    }

    free_pquota_control(driver->quota_ctrl);
    driver->quota_ctrl = NULL;
    free_overlay_options(driver->overlay_opts);
    driver->overlay_opts = NULL;

out:
    return ret;
}

static int check_lower_valid(const char *driver_home, const char *lower)
{
    int ret = 0;
    char *abs_path = NULL;

    abs_path = util_path_join(driver_home, lower);
    if (!util_dir_exists(abs_path)) {
        SYSERROR("Can't stat absolute layer:%s", abs_path);
        ret = -1;
        goto out;
    }

out:
    free(abs_path);
    return ret;
}

int overlay2_repair_lowers(const char *id, const char *parent, const struct graphdriver *driver)
{
    int ret = 0;
    char *layer_dir = NULL;
    char *lowers_str = NULL;
    char *lowers_repaired = NULL;
    char **lowers_arr = NULL;
    size_t lowers_size = 0;

    if (id == NULL || driver == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        goto out;
    }

    if (!util_dir_exists(layer_dir)) {
        SYSERROR("layer dir %s not exist", layer_dir);
        goto out;
    }

    lowers_str = read_layer_lower_file(layer_dir);
    lowers_arr = util_string_split(lowers_str, ':');
    lowers_size = util_array_len((const char **)lowers_arr);

    if (lowers_size != 0) {
        if (check_lower_valid(driver->home, lowers_arr[0]) == 0) {
            DEBUG("Try to repair layer %s, success check", id);
            ret = 0;
            goto out;
        }
    }

    if (parent == NULL) {
        if (mk_empty_directory(layer_dir) != 0) {
            ret = -1;
            goto out;
        }
    } else {
        lowers_repaired = get_lower(parent, driver->home);
        if (lowers_repaired == NULL) {
            ret = -1;
            goto out;
        }
        if (write_lowers(layer_dir, lowers_repaired) != 0) {
            ret = -1;
            goto out;
        }
    }

out:
    free(layer_dir);
    free(lowers_str);
    util_free_array(lowers_arr);
    free(lowers_repaired);
    return ret;
}

static int do_cal_layer_fs_info(const char *layer_diff, imagetool_fs_info *fs_info)
{
    int ret = 0;
    imagetool_fs_info_image_filesystems_element *fs_usage_tmp = NULL;
    int64_t total_size = 0;
    int64_t total_inodes = 0;

    fs_usage_tmp = util_common_calloc_s(sizeof(imagetool_fs_info_image_filesystems_element));
    if (fs_usage_tmp == NULL) {
        ERROR("Memory out");
        ret = -1;
        goto out;
    }

    fs_usage_tmp->timestamp = util_get_now_time_nanos();

    fs_usage_tmp->fs_id = util_common_calloc_s(sizeof(imagetool_fs_info_image_filesystems_fs_id));
    if (fs_usage_tmp->fs_id == NULL) {
        ERROR("Memory out");
        ret = -1;
        goto out;
    }
    fs_usage_tmp->fs_id->mountpoint = util_strdup_s(layer_diff);

    util_calculate_dir_size(layer_diff, 0, &total_size, &total_inodes);

    fs_usage_tmp->inodes_used = util_common_calloc_s(sizeof(imagetool_fs_info_image_filesystems_inodes_used));
    if (fs_usage_tmp->inodes_used == NULL) {
        ERROR("Memory out");
        ret = -1;
        goto out;
    }
    fs_usage_tmp->inodes_used->value = total_inodes;

    fs_usage_tmp->used_bytes = util_common_calloc_s(sizeof(imagetool_fs_info_image_filesystems_used_bytes));
    if (fs_usage_tmp->used_bytes == NULL) {
        ERROR("Memory out");
        ret = -1;
        goto out;
    }
    fs_usage_tmp->used_bytes->value = total_size;

    fs_info->image_filesystems = util_common_calloc_s(sizeof(imagetool_fs_info_image_filesystems_element *));
    if (fs_info->image_filesystems == NULL) {
        ERROR("Memory out");
        ret = -1;
        goto out;
    }
    fs_info->image_filesystems[0] = fs_usage_tmp;
    fs_usage_tmp = NULL;
    fs_info->image_filesystems_len = 1;

out:
    free_imagetool_fs_info_image_filesystems_element(fs_usage_tmp);
    return ret;
}

int overlay2_get_layer_fs_info(const char *id, const struct graphdriver *driver, imagetool_fs_info *fs_info)
{
    int ret = 0;
    char *layer_dir = NULL;
    char *layer_diff = NULL;

    if (id == NULL || fs_info == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    layer_dir = util_path_join(driver->home, id);
    if (layer_dir == NULL) {
        ERROR("Failed to join layer dir:%s", id);
        goto out;
    }

    if (!util_dir_exists(layer_dir)) {
        SYSERROR("layer dir %s not exist", layer_dir);
        goto out;
    }

    layer_diff = util_path_join(layer_dir, OVERLAY_LAYER_DIFF);
    if (layer_diff == NULL) {
        ERROR("Failed to join layer diff dir:%s", id);
        ret = -1;
        goto out;
    }

    if (do_cal_layer_fs_info(layer_diff, fs_info) != 0) {
        ERROR("Failed to cal layer diff :%s fs info", layer_diff);
        ret = -1;
        goto out;
    }

out:
    free(layer_dir);
    free(layer_diff);
    return ret;
}
