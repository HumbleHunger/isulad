/******************************************************************************
* Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
* iSulad licensed under the Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*     http://license.coscl.org.cn/MulanPSL2
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
* PURPOSE.:
* See the Mulan PSL v2 for more details.
* Author: gaohuatao
* Create: 2020-01-19
* Description: provide devicemapper graphdriver function definition
******************************************************************************/
#include "deviceset.h"

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/sysmacros.h>
#include <sys/mount.h>
#include <errno.h>
#include <isula_libutils/image_devmapper_device_info.h>
#include <isula_libutils/image_devmapper_deviceset_metadata.h>
#include <isula_libutils/image_devmapper_transaction.h>
#include <pthread.h>
#include <stdio.h>
#include <strings.h>
#include <sys/statfs.h>

#include "isula_libutils/log.h"
#include "err_msg.h"
#include "utils.h"
#include "wrapper_devmapper.h"
#include "devices_constants.h"
#include "libdevmapper.h"
#include "driver.h"
#include "constants.h"
#include "map.h"
#include "metadata_store.h"
#include "utils_array.h"
#include "utils_file.h"
#include "utils_fs.h"
#include "utils_string.h"
#include "utils_verify.h"
#include "selinux_label.h"

#define DM_LOG_FATAL 2
#define DM_LOG_DEBUG 7

static char *util_trim_prefice_string(char *str, const char *prefix)
{
    if (str == NULL || !util_has_prefix(str, prefix)) {
        return str;
    }

    char *begin = str + strlen(prefix);
    char *tmp = str;
    while ((*tmp++ = *begin++)) {
    }
    return str;
}

static int devmapper_parse_options(struct device_set *devset, const char **options, size_t options_len)
{
    size_t i = 0;

    if (devset == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    for (i = 0; options != NULL && i < options_len; i++) {
        char *dup = NULL;
        char *val = NULL;
        char *tmp_val = NULL;
        int ret = 0;
        int nret = 0;

        dup = util_strdup_s(options[i]);
        if (dup == NULL) {
            ERROR("Out of memory");
            return -1;
        }

        val = strchr(dup, '=');
        if (val == NULL) {
            ERROR("Unable to parse key/value option: '%s'", dup);
            isulad_set_error_message("Unable to parse key/value option: '%s'", dup);
            free(dup);
            return -1;
        }
        *val = '\0';
        val++;
        if (strcasecmp(dup, "dm.fs") == 0) {
            if (strcmp(val, "ext4") == 0) {
                free(devset->filesystem);
                devset->filesystem = util_strdup_s(val);
            } else {
                ERROR("Invalid filesystem: '%s': not supported", val);
                isulad_set_error_message("Invalid filesystem: '%s': not supported", val);
                ret = -1;
            }
        } else if (strcasecmp(dup, "dm.thinpooldev") == 0) {
            if (!util_valid_str(val)) {
                ERROR("Invalid thinpool device, it must not be empty");
                isulad_set_error_message("Invalid thinpool device, it must not be empty");
                ret = -1;
                goto out;
            }
            tmp_val = util_trim_prefice_string(val, "/dev/mapper/");
            devset->thin_pool_device = util_strdup_s(tmp_val);
        } else if (strcasecmp(dup, "dm.min_free_space") == 0) {
            long converted = 0;
            ret = util_parse_percent_string(val, &converted);
            if (ret != 0 || converted >= 100) {
                ERROR("Invalid min free space: '%s': %s", val, strerror(-ret));
                isulad_set_error_message("Invalid min free space: '%s': %s", val, strerror(-ret));
                ret = -1;
                goto out;
            }
            devset->min_free_space_percent = (uint32_t)converted;
        } else if (strcasecmp(dup, "dm.basesize") == 0) {
            int64_t converted = 0;
            ret = util_parse_byte_size_string(val, &converted);
            if (ret != 0) {
                ERROR("Invalid size: '%s': %s", val, strerror(-ret));
                isulad_set_error_message("Invalid size: '%s': %s", val, strerror(-ret));
                ret = -1;
                goto out;
            }
            if (converted <= 0) {
                ERROR("dm.basesize is lower than zero");
                isulad_set_error_message("dm.basesize is lower than zero");
                ret = -1;
                goto out;
            }
            devset->user_base_size = true;
            devset->base_fs_size = (uint64_t)converted;
        } else if (strcasecmp(dup, "dm.mkfsarg") == 0) {
            if (!util_valid_str(val)) {
                ERROR("Invalid dm.mkfsarg value");
                isulad_set_error_message("Invalid dm.mkfsarg value");
                ret = -1;
                goto out;
            }
            nret = util_array_append(&devset->mkfs_args, val);
            if (nret != 0) {
                ERROR("Out of memory");
                ret = -1;
                goto out;
            }
            devset->mkfs_args_len++;
        } else if (strcasecmp(dup, "dm.mountopt") == 0 || strcasecmp(dup, "devicemapper.mountopt") == 0) {
            if (!util_valid_str(val)) {
                ERROR("Invalid dm.mountopt or devicemapper.mountopt value");
                isulad_set_error_message("Invalid dm.mountopt or devicemapper.mountopt value");
                ret = -1;
                goto out;
            }
            devset->mount_options = util_strdup_s(val);
        } else {
            ERROR("devicemapper: unknown option: '%s'", dup);
            isulad_set_error_message("devicemapper: unknown option: '%s'", dup);
            ret = -1;
        }
out:
        free(dup);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static char *metadata_dir(const struct device_set *devset)
{
    return util_path_join(devset->root, "metadata");
}

static char *transaction_meta_file(struct device_set *devset)
{
    char *dir = NULL;
    char *file = NULL;

    dir = metadata_dir(devset);
    if (dir == NULL) {
        ERROR("Failed to get meta data directory");
        return NULL;
    }

    file = util_path_join(dir, TRANSACTION_METADATA);
    free(dir);

    return file;
}

static char *deviceset_meta_file(struct device_set *devset)
{
    char *dir = NULL;
    char *file = NULL;

    dir = metadata_dir(devset);
    if (dir == NULL) {
        ERROR("Failed to get meta data directory");
        return NULL;
    }

    file = util_path_join(dir, DEVICE_SET_METAFILE);
    free(dir);

    return file;
}

// get_dm_name return value format:container-253:0-409697-401641a00390ccd2b21eb464f5eb5a7b735c3731b717e7bffafe65971f4cb498
static char *get_dm_name(struct device_set *devset, const char *hash)
{
    int nret = 0;
    char buff[PATH_MAX] = { 0 };

    if (hash == NULL) {
        ERROR("Invalid input param");
        return NULL;
    }

    nret = snprintf(buff, sizeof(buff), "%s-%s", devset->device_prefix, strcmp(hash, "") == 0 ? "base" : hash);
    if (nret < 0 || (size_t)nret >= sizeof(buff)) {
        return NULL;
    }

    return util_strdup_s(buff);
}

// get_dev_name return value fromat:/dev/mapper/container-253:0-409697-401641a00390ccd2b21eb464f5eb5a7b735c3731b717e7bffafe65971f4cb498
static char *get_dev_name(const char *name)
{
    return util_string_append(name, DEVMAPPER_DECICE_DIRECTORY);
}

char *dev_name(struct device_set *devset, image_devmapper_device_info *info)
{
    char *res_str = NULL;
    char *dm_name = NULL;

    dm_name = get_dm_name(devset, info->hash);
    if (dm_name == NULL) {
        ERROR("devmapper: get dm device name with hash:%s failed", info->hash);
        return NULL;
    }

    res_str = get_dev_name(dm_name);
    free(dm_name);
    return res_str;
}

static char *get_pool_dev_name(struct device_set *devset)
{
    char *pool_name = NULL;
    char *dev_name = NULL;

    pool_name = util_strdup_s(devset->thin_pool_device);
    if (pool_name == NULL) {
        ERROR("Failed to get pool name");
        goto out;
    }

    dev_name = get_dev_name(pool_name);
    if (dev_name == NULL) {
        ERROR("devmapper: pool device name is NULL");
        goto out;
    }

out:
    free(pool_name);
    return dev_name;
}

static int deactivate_device_mode(struct device_set *devset, image_devmapper_device_info *dev_info)
{
    int ret = 0;
    int nret = 0;
    char *dm_name = NULL;
    struct dm_info dinfo;

    if (devset == NULL || dev_info == NULL) {
        ERROR("Invalid input params to deactivate device");
        return -1;
    }

    dm_name = get_dm_name(devset, dev_info->hash);
    if (dm_name == NULL) {
        ERROR("devmapper: get dm device name with hash:%s failed", dev_info->hash);
        ret = -1;
        goto free_out;
    }

    if (dev_get_info(&dinfo, dm_name) != 0) {
        ERROR("devmapper: get device info failed");
        ret = -1;
        goto free_out;
    }

    if (dinfo.exists == 0) {
        DEBUG("devmapper: device has exited, no need to remove again");
        goto free_out;
    }

    nret = dev_remove_device_deferred(dm_name);
    if (nret != 0) {
        ERROR("devmapper: remove device:%s failed, err:%s", dm_name, dev_strerror(nret));
        if (nret == ERR_ENXIO) {
            WARN("devmapper: device %s has gone", dm_name);
            goto free_out;
        }
        ret = -1;
        goto free_out;
    }

free_out:
    free(dm_name);
    return ret;
}

static int deactivate_device(struct device_set *devset, image_devmapper_device_info *dev_info)
{
    return deactivate_device_mode(devset, dev_info);
}

static int pool_status(struct device_set *devset, uint64_t *total_size_in_sectors, uint64_t *transaction_id,
                       uint64_t *data_used, uint64_t *data_total, uint64_t *metadata_used, uint64_t *metadata_total)
{
    int ret = 0;
    uint64_t start;
    uint64_t length;
    char *target_type = NULL;
    char *params = NULL;
    char *name = NULL;

    if (!total_size_in_sectors || !transaction_id || !data_used || !data_total || !metadata_used || !metadata_total) {
        ERROR("devmapper: input params is NULL");
        return -1;
    }

    name = util_strdup_s(devset->thin_pool_device);
    if (name == NULL) {
        ERROR("devmapper: dup str failed");
        ret = -1;
        goto out;
    }

    if (dev_get_status(&start, &length, &target_type, &params, name) != 0) {
        ERROR("devmapper: get dev status for pool name is %s", name);
        ret = -1;
        goto out;
    }

    *total_size_in_sectors = length;
    if (sscanf(params, "%lu %lu/%lu %lu/%lu", transaction_id, metadata_used, metadata_total, data_used, data_total) !=
        5) {
        ERROR("devmapper: sscanf device status params failed");
        ret = -1;
        goto out;
    }

out:
    free(name);
    free(target_type);
    free(params);
    return ret;
}

static bool thin_pool_exists(struct device_set *devset, const char *pool_name)
{
    bool exist = true;
    struct dm_info *dinfo = NULL;
    uint64_t start, length;
    char *target_type = NULL;
    char *params = NULL;

    dinfo = util_common_calloc_s(sizeof(struct dm_info));
    if (dinfo == NULL) {
        ERROR("Out of memory");
        return false;
    }

    if (dev_get_info(dinfo, pool_name) != 0) {
        ERROR("devmapper: get dev info with deferred failed");
        exist = false;
        goto out;
    }

    if (dinfo->exists == 0) {
        ERROR("devmapper: thin pool not exists");
        exist = false;
        goto out;
    }

    if (dev_get_status(&start, &length, &target_type, &params, pool_name) != 0 ||
        strcmp(target_type, "thin-pool") != 0) {
        ERROR("Get thin pool status failed or not match thin-pool type");
        exist = false;
    }

out:
    free(dinfo);
    free(target_type);
    free(params);
    return exist;
}

static image_devmapper_device_info *load_metadata(const struct device_set *devset, const char *hash)
{
    image_devmapper_device_info *info = NULL;
    char metadata_file[PATH_MAX] = { 0 };
    char *metadata_path = NULL;
    int nret = 0;
    parser_error err = NULL;

    if (hash == NULL) {
        return NULL;
    }

    metadata_path = metadata_dir(devset);
    if (metadata_path == NULL) {
        ERROR("Failed to get meta data directory");
        goto out;
    }

    nret = snprintf(metadata_file, sizeof(metadata_file), "%s/%s", metadata_path, util_valid_str(hash) ? hash : "base");
    if (nret < 0 || (size_t)nret >= sizeof(metadata_file)) {
        ERROR("Failed to snprintf metadata file path with hash:%s, path is too long", hash);
        goto out;
    }

    if (!util_file_exists(metadata_file)) {
        WARN("No such file:%s, need not to load", metadata_file);
        goto out;
    }

    info = image_devmapper_device_info_parse_file(metadata_file, NULL, &err);
    if (info == NULL) {
        SYSERROR("Load metadata file:%s failed:%s", metadata_file, err);
        goto out;
    }

    if (!util_valid_str(info->hash)) {
        free(info->hash);
        info->hash = util_strdup_s(hash);
    }

    if (info->device_id > MAX_DEVICE_ID) {
        ERROR("devmapper: device id:%d out of limits, to be ignored", info->device_id);
        free_image_devmapper_device_info(info);
        info = NULL;
        goto out;
    }

out:
    free(metadata_path);
    free(err);
    return info;
}

static void run_blkid_get_uuid(void *args)
{
    char **tmp_args = (char **)args;
    const size_t CMD_ARGS_NUM = 6;

    if (util_array_len((const char **)tmp_args) != (size_t)CMD_ARGS_NUM) {
        COMMAND_ERROR("Blkid get uuid need six args");
        exit(1);
    }

    execvp(tmp_args[0], tmp_args);
}

static char *get_device_uuid(const char *dev_fname)
{
    char **args = NULL;
    char *stdout_msg = NULL;
    char *stderr_msg = NULL;
    char *uuid = NULL;

    if (dev_fname == NULL) {
        return NULL;
    }

    args = (char **)util_common_calloc_s(sizeof(char *) * 7);
    if (args == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    args[0] = util_strdup_s("blkid");
    args[1] = util_strdup_s("-s");
    args[2] = util_strdup_s("UUID");
    args[3] = util_strdup_s("-o");
    args[4] = util_strdup_s("value");
    args[5] = util_strdup_s(dev_fname);
    if (!util_exec_cmd(run_blkid_get_uuid, args, NULL, &stdout_msg, &stderr_msg)) {
        ERROR("Unexpected command output %s with error: %s", stdout_msg, stderr_msg);
        goto free_out;
    }

    if (stdout_msg == NULL) {
        ERROR("call blkid -s UUID -o value %s no stdout", dev_fname);
        goto free_out;
    }
    util_trim_newline(stdout_msg);
    stdout_msg = util_trim_space(stdout_msg);
    uuid = util_strdup_s(stdout_msg);

free_out:
    util_free_array(args);
    free(stdout_msg);
    free(stderr_msg);
    return uuid;
}

static void run_grow_rootfs(void *args)
{
    char **tmp_args = (char **)args;
    const size_t CMD_ARGS_NUM = 2;

    if (util_array_len((const char **)tmp_args) != CMD_ARGS_NUM) {
        COMMAND_ERROR("grow rootfs need three args");
        exit(1);
    }

    execvp(tmp_args[0], tmp_args);
}

static int exec_grow_fs_command(const char *command, const char *dev_fname)
{
    int ret = 0;
    char **args = NULL;
    char *stdout_msg = NULL;
    char *stderr_msg = NULL;

    if (command == NULL || dev_fname == NULL) {
        ERROR("devmapper: invalid input params to exec grow fs command");
        return -1;
    }

    args = (char **)util_common_calloc_s(sizeof(char *) * 3);
    if (args == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto free_out;
    }

    args[0] = util_strdup_s(command);
    args[1] = util_strdup_s(dev_fname);
    if (!util_exec_cmd(run_grow_rootfs, args, NULL, &stdout_msg, &stderr_msg)) {
        ERROR("Grow rootfs failed, unexpected command output %s with error: %s", stdout_msg, stderr_msg);
        ret = -1;
        goto free_out;
    }

free_out:
    util_free_array(args);
    free(stdout_msg);
    free(stderr_msg);
    return ret;
}

static devmapper_device_info_t *lookup_device(struct device_set *devset, const char *hash)
{
    devmapper_device_info_t *device_info = NULL;

    device_info = metadata_store_get(hash, devset->meta_store);
    if (device_info == NULL) {
        image_devmapper_device_info *info = NULL;
        info = load_metadata(devset, hash);
        if (info == NULL) {
            WARN("No such device file:%s in metadata dir, stop to lookup", hash);
            goto out;
        }

        if (!metadata_store_add(hash, info, devset->meta_store)) {
            ERROR("devmapper: add device %s to local store map failed", hash);
            free_image_devmapper_device_info(info);
            goto out;
        }
        device_info = metadata_store_get(hash, devset->meta_store);
    }

out:
    return device_info;
}

static uint64_t get_base_device_size(struct device_set *devset)
{
    uint64_t res = 0;
    devmapper_device_info_t *device_info = NULL;

    device_info = lookup_device(devset, "base");
    if (device_info == NULL) {
        ERROR("No such device:\"base\"");
        return 0;
    }

    res = device_info->info->size;
    devmapper_device_info_ref_dec(device_info);

    return res;
}

static bool util_valid_device_hash(const char *hash)
{
    char *patten = "^[a-f0-9]{64}$";

    if (hash == NULL) {
        ERROR("invalid NULL param");
        return false;
    }

    if (strcmp(hash, "base") == 0) {
        return true;
    }

    return util_reg_match(patten, hash) == 0;
}

static int device_file_walk(struct device_set *devset)
{
    int ret = 0;
    char *metadir = NULL;
    DIR *dp = NULL;
    struct dirent *entry = NULL;
    struct stat st;
    devmapper_device_info_t *device_info = NULL;

    metadir = metadata_dir(devset);
    if (metadir == NULL) {
        ERROR("Failed to get meta data directory");
        return -1;
    }

    dp = opendir(metadir);
    if (dp == NULL) {
        ERROR("devmapper: open dir %s failed", metadir);
        ret = -1;
        goto out;
    }

    while ((entry = readdir(dp)) != NULL) {
        int pathname_len;
        char fname[PATH_MAX] = { 0 };

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        (void)memset(fname, 0, sizeof(fname));
        pathname_len = snprintf(fname, PATH_MAX, "%s/%s", metadir, entry->d_name);
        if (pathname_len < 0 || pathname_len >= PATH_MAX) {
            ERROR("Pathname too long");
            continue;
        }

        if (strcmp(entry->d_name, DEVICE_SET_METAFILE) == 0 || strcmp(entry->d_name, TRANSACTION_METADATA) == 0) {
            continue;
        }

        if (stat(fname, &st) != 0) {
            ERROR("devmapper: get %s stat error:%s", fname, strerror(errno));
            ret = -1;
            goto out;
        }

        if (S_ISDIR(st.st_mode)) {
            DEBUG("Walk metadata file to skip dir:%s", fname);
            continue;
        }

        if (!util_valid_device_hash(entry->d_name)) {
            ERROR("Remove device metadata file:%s related invalid device file", entry->d_name);
            if (util_path_remove(fname) != 0) {
                ERROR("Failed to delete device metadata file:%s with invalid name", fname);
            }
            continue;
        }

        device_info = lookup_device(devset, entry->d_name);
        if (device_info == NULL) {
            ERROR("Lookup device file:%s error, please check the file", entry->d_name);
            ret = -1;
            goto out;
        }
        devmapper_device_info_ref_dec(device_info);
    }

out:
    if (dp != NULL) {
        closedir(dp);
    }
    free(metadir);
    return ret;
}

static void mark_device_id_used(struct device_set *devset, int device_id)
{
    int mask;
    int value = 0;
    int *value_ptr = NULL;
    int key = device_id / 8;

    mask = 1 << (device_id % 8);

    value_ptr = map_search(devset->device_id_map, &key);
    if (value_ptr == NULL) {
        value = value | mask;
        if (!map_insert(devset->device_id_map, &key, &value)) {
            WARN("devmapper: map insert failed");
        }
    } else {
        value = *value_ptr | mask;
        if (!map_replace(devset->device_id_map, &key, &value)) {
            WARN("devmapper: map replace failed");
        }
    }
}

static void mark_device_id_free(struct device_set *devset, int device_id)
{
    int mask = 0;
    int value = 0;
    int *value_ptr = NULL;
    int key = device_id / 8;
    bool res;

    mask = ~(1 << (device_id % 8));

    value_ptr = map_search(devset->device_id_map, &key);
    if (value_ptr == NULL) {
        value = value & mask;
        res = map_insert(devset->device_id_map, &key, &value);
        if (!res) {
            WARN("devmapper: map insert failed");
        }
    } else {
        value = *value_ptr & mask;
        res = map_replace(devset->device_id_map, &key, &value);
        if (!res) {
            WARN("devmapper: map replace failed");
        }
    }
}

static void construct_device_id_map(struct device_set *devset)
{
    char **dev_arr = NULL;
    size_t dev_arr_len = 0;
    size_t i = 0;
    devmapper_device_info_t *device_info = NULL;

    dev_arr = metadata_store_list_hashes(devset->meta_store);
    dev_arr_len = util_array_len((const char **)dev_arr);

    for (i = 0; i < dev_arr_len; i++) {
        device_info = lookup_device(devset, dev_arr[i]);
        if (device_info == NULL) {
            WARN("devmapper: lookup device %s failed, just skip", dev_arr[i]);
            continue;
        }
        mark_device_id_used(devset, device_info->info->device_id);
        devmapper_device_info_ref_dec(device_info);
    }
    util_free_array(dev_arr);
}

static void count_deleted_devices(struct device_set *devset)
{
    char **dev_arr = NULL;
    size_t dev_arr_len = 0;
    size_t i = 0;
    devmapper_device_info_t *device_info = NULL;

    dev_arr = metadata_store_list_hashes(devset->meta_store);
    dev_arr_len = util_array_len((const char **)dev_arr);

    for (i = 0; i < dev_arr_len; i++) {
        device_info = lookup_device(devset, dev_arr[i]);
        if (device_info == NULL) {
            WARN("Lookup device %s failed, just skip marking deleted", dev_arr[i]);
            continue;
        }
        if (device_info->info->deleted) {
            devset->nr_deleted_devices++;
        }
        devmapper_device_info_ref_dec(device_info);
    }
    util_free_array(dev_arr);
}

static int remove_transaction_metadata(struct device_set *devset)
{
    int ret = 0;
    char *fname = NULL;

    fname = transaction_meta_file(devset);
    if (fname == NULL) {
        ERROR("devmapper: get transaction file abs path failed");
        return -1;
    }

    if (util_path_remove(fname) != 0) {
        ERROR("devmapper: remove transaction metadata file %s failed", fname);
        ret = -1;
    }
    free(fname);

    return ret;
}

static char *metadata_file(struct device_set *devset, const char *hash)
{
    char *full_path = NULL;
    char *dir = NULL;

    if (hash == NULL) {
        ERROR("devmapper: get metadata file param is null");
        return NULL;
    }

    dir = metadata_dir(devset);
    if (dir == NULL) {
        ERROR("devmapper: get metadata dir of device %s failed", hash);
        return NULL;
    }

    full_path = util_path_join(dir, hash);
    free(dir);

    return full_path;
}

static int remove_metadata(struct device_set *devset, const char *hash)
{
    int ret = 0;
    char *fname = NULL;

    fname = metadata_file(devset, hash);
    if (fname == NULL) {
        ERROR("devmapper: get device %s metadata file full path failed", hash);
        return -1;
    }

    DEBUG("devmapper: start to remove metadata file:%s", fname);
    if (util_path_remove(fname) != 0) {
        ERROR("devmapper: remove metadata file %s failed", hash);
        ret = -1;
    }
    free(fname);

    return ret;
}

static int load_transaction_metadata(struct device_set *devset)
{
    image_devmapper_transaction *trans = NULL;
    char fname[PATH_MAX] = { 0 };
    parser_error err = NULL;
    int ret = 0;
    int nret = 0;

    nret = snprintf(fname, sizeof(fname), "%s/metadata/%s", devset->root, TRANSACTION_METADATA);
    if (nret < 0 || (size_t)nret >= sizeof(fname)) {
        ERROR("devmapper: failed make transaction-metadata full path");
        ret = -1;
        goto out;
    }

    if (!util_file_exists(fname)) {
        devset->metadata_trans->open_transaction_id = devset->transaction_id;
        WARN("There is no active transaction, may be during upgrade");
        goto out;
    }

    trans = image_devmapper_transaction_parse_file(fname, NULL, &err);
    if (trans == NULL) {
        SYSERROR("Load transaction metadata file:%s failed:%s", fname, err);
        ret = -1;
        goto out;
    }

    if (!util_valid_str(trans->device_hash)) {
        free(trans->device_hash);
        trans->device_hash = util_strdup_s("base");
    }

    free_image_devmapper_transaction(devset->metadata_trans);
    devset->metadata_trans = trans;

out:
    free(err);
    return ret;
}

static void rollback_transaction(struct device_set *devset)
{
    char *pool_dev = NULL;

    pool_dev = get_pool_dev_name(devset);
    if (pool_dev == NULL) {
        WARN("devmapper: get pool device name failed");
    }

    if (dev_delete_device(pool_dev, devset->metadata_trans->device_id) != 0) {
        WARN("devmapper: unable to delete device:%s", pool_dev);
    }

    if (remove_metadata(devset, devset->metadata_trans->device_hash) != 0) {
        WARN("devmapper: unable to remove metadata");
    } else {
        mark_device_id_free(devset, devset->metadata_trans->device_id);
    }

    if (metadata_store_remove(devset->metadata_trans->device_hash, devset->meta_store) != 0) {
        WARN("devmapper: remove unused device from store failed");
    }

    if (remove_transaction_metadata(devset) != 0) {
        WARN("devmapper: unable to remove transaction meta file");
    }

    free(pool_dev);
}

static int process_pending_transaction(struct device_set *devset)
{
    int ret = 0;

    if (devset == NULL || devset->metadata_trans == NULL) {
        ERROR("devmapper: device set or tansaction is NULL");
        return -1;
    }

    if (load_transaction_metadata(devset) != 0) {
        ERROR("devmapper: load transaction-metadata failed, process pending transaction terminate");
        ret = -1;
        goto out;
    }

    // If there was open transaction but pool transaction ID is same
    // as open transaction ID, nothing to roll back.
    if (devset->transaction_id == devset->metadata_trans->open_transaction_id) {
        DEBUG("devmapper: nothing to roll back");
        goto out;
    }

    // If open transaction ID is less than pool transaction ID, something
    // is wrong. Bail out.
    if (devset->transaction_id > devset->metadata_trans->open_transaction_id) {
        WARN("devmapper: Open Transaction id %ld is less than pool transaction id %ld",
             devset->metadata_trans->open_transaction_id, devset->transaction_id);
        goto out;
    }
    rollback_transaction(devset);
    devset->metadata_trans->open_transaction_id = devset->transaction_id;

out:
    return ret;
}

static void cleanup_deleted_devices(struct device_set *devset)
{
    char **idsarray = NULL;
    size_t ids_len;
    size_t i;

    if (devset->nr_deleted_devices == 0) {
        DEBUG("devmapper: no devices to delete");
        return;
    }

    idsarray = metadata_store_list_hashes(devset->meta_store);
    if (idsarray == NULL) {
        WARN("devmapper: get metadata store list failed");
        return;
    }
    ids_len = util_array_len((const char **)idsarray);

    for (i = 0; i < ids_len; i++) {
        devmapper_device_info_t *device_info = NULL;

        device_info = lookup_device(devset, idsarray[i]);
        if (device_info == NULL || device_info->info == NULL) {
            DEBUG("devmapper: no such device with hash(%s), just skip cleanup", idsarray[i]);
            continue;
        }

        if (!device_info->info->deleted) {
            devmapper_device_info_ref_dec(device_info);
            device_info = NULL;
            DEBUG("No need to delete device with hash(%s)", idsarray[i]);
            continue;
        }

        devmapper_device_info_ref_dec(device_info);
        device_info = NULL;
        if (delete_device(idsarray[i], false, devset) != 0) {
            WARN("devmapper:Deletion of device: \"%s\" failed", idsarray[i]);
        }
    }

    util_free_array_by_len(idsarray, ids_len);
}

static int init_metadata(struct device_set *devset, const char *pool_name)
{
    int ret = 0;
    uint64_t total_size_in_sectors, transaction_id, data_used;
    uint64_t data_total, metadata_used, metadata_total;

    if (pool_status(devset, &total_size_in_sectors, &transaction_id, &data_used, &data_total, &metadata_used,
                    &metadata_total) != 0) {
        ERROR("devmapper: get pool %s status failed", pool_name);
        ret = -1;
        goto out;
    }

    devset->transaction_id = transaction_id;

    if (device_file_walk(devset) != 0) {
        ERROR("devmapper: Failed to load device files");
        ret = -1;
        goto out;
    }

    construct_device_id_map(devset);
    count_deleted_devices(devset);
    if (process_pending_transaction(devset) != 0) {
        ERROR("devmapper: process pending transaction failed");
        ret = -1;
        goto out;
    }

    cleanup_deleted_devices(devset);

out:
    return ret;
}

static int load_deviceset_metadata(struct device_set *devset)
{
    int ret = 0;
    image_devmapper_deviceset_metadata *deviceset_meta = NULL;
    parser_error err = NULL;
    char *meta_file = NULL;

    meta_file = deviceset_meta_file(devset);
    if (meta_file == NULL) {
        ERROR("Get device metadata file %s full path failed", DEVICE_SET_METAFILE);
        return -1;
    }

    if (!util_file_exists(meta_file)) {
        DEBUG("devmapper: device metadata file %s not exist", DEVICE_SET_METAFILE);
        goto out;
    }

    deviceset_meta = image_devmapper_deviceset_metadata_parse_file(meta_file, NULL, &err);
    if (deviceset_meta == NULL) {
        SYSERROR("Load deviceset metadata file:%s failed:%s", meta_file, err);
        ret = -1;
        goto out;
    }
    devset->next_device_id = deviceset_meta->next_device_id;
    devset->base_device_filesystem = util_strdup_s(deviceset_meta->base_device_filesystem);
    devset->base_device_uuid = util_strdup_s(deviceset_meta->base_device_uuid);

out:
    free(err);
    free_image_devmapper_deviceset_metadata(deviceset_meta);
    free(meta_file);
    return ret;
}

static bool is_device_id_free(struct device_set *devset, int device_id)
{
    int mask = 0;
    int value = 0;
    int *value_ptr = NULL;
    int key = device_id / 8;

    mask = 1 << (device_id % 8);
    value_ptr = map_search(devset->device_id_map, &key);

    return value_ptr ? (*value_ptr & mask) == 0 : (value & mask) == 0;
}

static void inc_next_device_id(struct device_set *devset)
{
    devset->next_device_id = (devset->next_device_id + 1) & MAX_DEVICE_ID;
}

static int get_next_free_device_id(struct device_set *devset, int *next_id)
{
    int i = 0;

    if (next_id == NULL) {
        ERROR("Invalid input param");
        return -1;
    }

    inc_next_device_id(devset);
    for (i = 0; i <= MAX_DEVICE_ID; i++) {
        if (is_device_id_free(devset, devset->next_device_id)) {
            mark_device_id_used(devset, devset->next_device_id);
            *next_id = devset->next_device_id;
            return 0;
        }
        inc_next_device_id(devset);
    }

    return -1;
}

static int pool_has_free_space(struct device_set *devset)
{
    int ret = 0;
    uint64_t total_size_in_sectors, transaction_id, data_used;
    uint64_t data_total, metadata_used, metadata_total;
    uint64_t min_free_data, data_free, min_free_metadata, metadata_free;

    if (devset->min_free_space_percent == 0) {
        DEBUG("devmapper: min free space percent is zero");
        goto out;
    }

    if (pool_status(devset, &total_size_in_sectors, &transaction_id, &data_used, &data_total, &metadata_used,
                    &metadata_total) != 0) {
        ERROR("devmapper: get pool status failed");
        ret = -1;
        goto out;
    }

    min_free_data = (data_total * (uint64_t)devset->min_free_space_percent) / 100;
    if (min_free_data < 1) {
        min_free_data = 1;
    }

    data_free = data_total - data_used;
    if (data_free < min_free_data) {
        ERROR("devmapper: Thin Pool has %lu free data blocks which is less than minimum required "
              "%lu free data blocks. Create more free space in thin pool or use dm.min_free_space option to change behavior",
              data_total - data_used, min_free_data);
        isulad_set_error_message(
            "devmapper: Thin Pool has %lu free data blocks which is less than minimum required "
            "%lu free data blocks. Create more free space in thin pool or use dm.min_free_space option to change behavior",
            data_total - data_used, min_free_data);
        ret = -1;
        goto out;
    }

    min_free_metadata = (metadata_total * (uint64_t)devset->min_free_space_percent) / 100;
    if (min_free_metadata < 1) {
        min_free_metadata = 1;
    }

    metadata_free = metadata_total - metadata_used;
    if (metadata_free < min_free_metadata) {
        ERROR("devmapper: Thin Pool has %lu free metadata blocks "
              "which is less than minimum required %lu free metadata blocks. "
              "Create more free metadata space in thin pool or use dm.min_free_space option to change behavior",
              metadata_total - metadata_used, min_free_metadata);
        isulad_set_error_message(
            "devmapper: Thin Pool has %lu free metadata blocks "
            "which is less than minimum required %lu free metadata blocks. "
            "Create more free metadata space in thin pool or use dm.min_free_space option to change behavior",
            metadata_total - metadata_used, min_free_metadata);
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int save_metadata(struct device_set *devset, image_devmapper_device_info *info)
{
    int ret = 0;
    char *metadata_json = NULL;
    char *fname = NULL;
    parser_error err = NULL;

    if (info == NULL) {
        ERROR("devmapper: input param is null");
        return -1;
    }

    fname = metadata_file(devset, info->hash);
    if (fname == NULL) {
        ERROR("devmapper: get device %s metadata file full path failed", info->hash);
        ret = -1;
        goto out;
    }

    metadata_json = image_devmapper_device_info_generate_json(info, NULL, &err);
    if (metadata_json == NULL) {
        ERROR("devmapper: generate metadata json error %s", err);
        ret = -1;
        goto out;
    }

    if (util_atomic_write_file(fname, metadata_json, strlen(metadata_json), DEFAULT_SECURE_FILE_MODE, true) != 0) {
        ERROR("failed write process.json");
        ret = -1;
        goto out;
    }

out:
    free(err);
    free(metadata_json);
    free(fname);
    return ret;
}

static int save_transaction_metadata(struct device_set *devset)
{
    int ret = 0;
    int nret = 0;
    char *trans_json = NULL;
    char fname[PATH_MAX] = { 0 };
    parser_error err = NULL;

    nret = snprintf(fname, sizeof(fname), "%s/metadata/%s", devset->root, TRANSACTION_METADATA);
    if (nret < 0 || (size_t)nret >= sizeof(fname)) {
        ERROR("devmapper: failed make transaction-metadata full path");
        ret = -1;
        goto out;
    }

    trans_json = image_devmapper_transaction_generate_json(devset->metadata_trans, NULL, &err);
    if (trans_json == NULL) {
        ERROR("devmapper: generate transaction json error %s", err);
        ret = -1;
        goto out;
    }

    if (util_atomic_write_file(fname, trans_json, strlen(trans_json), DEFAULT_SECURE_FILE_MODE, true) != 0) {
        ERROR("failed write process.json");
        ret = -1;
        goto out;
    }

out:
    free(err);
    free(trans_json);
    return ret;
}

static int save_deviceset_matadata(struct device_set *devset)
{
    int ret = 0;
    image_devmapper_deviceset_metadata *devset_metadata = NULL;
    char *metadata_json = NULL;
    char *fname = NULL;
    parser_error err = NULL;

    fname = deviceset_meta_file(devset);
    if (fname == NULL) {
        ERROR("devmapper: get deviceset metadata file full path failed");
        ret = -1;
        goto free_out;
    }

    devset_metadata = util_common_calloc_s(sizeof(image_devmapper_deviceset_metadata));
    if (devset_metadata == NULL) {
        ERROR("devmapper: Out of memory");
        ret = -1;
        goto free_out;
    }

    devset_metadata->base_device_filesystem = util_strdup_s(devset->base_device_filesystem);
    devset_metadata->base_device_uuid = util_strdup_s(devset->base_device_uuid);
    devset_metadata->next_device_id = devset->next_device_id;

    metadata_json = image_devmapper_deviceset_metadata_generate_json(devset_metadata, NULL, &err);
    if (metadata_json == NULL) {
        ERROR("devmapper: generate deviceset metadata json error %s", err);
        ret = -1;
        goto free_out;
    }

    if (util_atomic_write_file(fname, metadata_json, strlen(metadata_json), DEFAULT_SECURE_FILE_MODE, true) != 0) {
        ERROR("failed write process.json");
        ret = -1;
        goto free_out;
    }

free_out:
    free_image_devmapper_deviceset_metadata(devset_metadata);
    free(err);
    free(metadata_json);
    free(fname);
    return ret;
}

static int open_transaction(struct device_set *devset, const char *hash, int id)
{
    int ret = 0;

    if (devset->metadata_trans == NULL || hash == NULL) {
        ERROR("devmapper: open transaction params null");
        return -1;
    }
    devset->metadata_trans->open_transaction_id = devset->transaction_id + 1;
    free(devset->metadata_trans->device_hash);
    devset->metadata_trans->device_hash = util_strdup_s(hash);
    devset->metadata_trans->device_id = id;

    if (save_transaction_metadata(devset) != 0) {
        ERROR("devmapper: Error saving transaction metadata");
        ret = -1;
    }

    return ret;
}

static int refresh_transaction(struct device_set *devset, int id)
{
    int ret = 0;

    if (devset->metadata_trans == NULL) {
        ERROR("devmapper: refresh transaction params null");
        ret = -1;
        goto out;
    }

    devset->metadata_trans->device_id = id;
    if (save_transaction_metadata(devset) != 0) {
        ERROR("devmapper: Error saving transaction metadata");
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int update_pool_transaction_id(struct device_set *devset)
{
    int ret = 0;
    char *pool_name = NULL;

    pool_name = get_pool_dev_name(devset);
    if (pool_name == NULL) {
        ERROR("devmapper: get pool device name failed");
        ret = -1;
        goto out;
    }

    if (dev_set_transaction_id(pool_name, devset->transaction_id, devset->metadata_trans->open_transaction_id) != 0) {
        ERROR("devmapper: set transaction id failed with pool name:%s", pool_name);
        ret = -1;
        goto out;
    }
    devset->transaction_id = devset->metadata_trans->open_transaction_id;

out:
    free(pool_name);
    return ret;
}

static int close_transaction(struct device_set *devset)
{
    return update_pool_transaction_id(devset);
}

static int unregister_device(struct device_set *devset, const char *hash)
{
    if (!metadata_store_remove(hash, devset->meta_store)) {
        ERROR("devmapper: remove metadata store %s failed", hash);
        return -1;
    }

    if (remove_metadata(devset, hash) != 0) {
        ERROR("devmapper: remove metadata file %s failed", hash);
        return -1;
    }

    return 0;
}

static image_devmapper_device_info *register_device(struct device_set *devset, int id, const char *hash, uint64_t size,
                                                    uint64_t transaction_id)
{
    image_devmapper_device_info *info = NULL;

    info = util_common_calloc_s(sizeof(image_devmapper_device_info));
    if (info == NULL) {
        ERROR("devmapper: Out of memory");
        return NULL;
    }

    info->device_id = id;
    info->size = size;
    info->transaction_id = transaction_id;
    info->initialized = false;
    info->hash = util_strdup_s(hash);
    info->deleted = false;

    if (!metadata_store_add(hash, info, devset->meta_store)) {
        ERROR("devmapper: metadata store add failed hash %s", hash);
        free_image_devmapper_device_info(info);
        goto out;
    }

    if (save_metadata(devset, info) != 0) {
        ERROR("devmapper: save metadata of device %s failed", hash);
        if (!metadata_store_remove(hash, devset->meta_store)) {
            ERROR("devmapper: metadata file %s store remove failed", hash);
        }
        goto out;
    }

    return info;

out:
    return NULL;
}

static image_devmapper_device_info *create_register_device(struct device_set *devset, const char *hash)
{
    int nret = 0;
    int ret = 0;
    int device_id = 0;
    char *pool_dev = NULL;
    image_devmapper_device_info *info = NULL;

    if (get_next_free_device_id(devset, &device_id) != 0) {
        ERROR("devmapper: cannot get next free device id");
        return NULL;
    }

    if (open_transaction(devset, hash, device_id) != 0) {
        ERROR("devmapper: Error opening transaction hash = %s deviceID = %d", hash, device_id);
        ret = -1;
        mark_device_id_free(devset, device_id);
        goto out;
    }

    pool_dev = get_pool_dev_name(devset);
    if (pool_dev == NULL) {
        ERROR("devmapper: get pool device name failed");
        ret = -1;
        goto out;
    }

    do {
        nret = dev_create_device(pool_dev, device_id);
        if (nret != 0) {
            ERROR("devmapper: create device with id:%d failed, err:%s", device_id, dev_strerror(nret));
            if (nret == ERR_DEVICE_ID_EXISTS) {
                ERROR("devmapper: device id %d exists in pool but it is supposed to be unused", device_id);
                if (get_next_free_device_id(devset, &device_id) != 0) {
                    ERROR("devmapper: cannot get next free device id");
                    ret = -1;
                    goto out;
                }

                if (refresh_transaction(devset, device_id) != 0) {
                    DEBUG("devmapper: Error refres open transaction deviceID %s = %d", hash, device_id);
                }
                continue;
            }
            ret = -1;
            mark_device_id_free(devset, device_id);
            goto out;
        }
        break;
    } while (true);

    info = register_device(devset, device_id, hash, devset->base_fs_size, devset->metadata_trans->open_transaction_id);
    if (info == NULL) {
        ERROR("devmapper: register device %d failed, start to delete device", device_id);
        ret = -1;
        if (dev_delete_device(pool_dev, device_id) != 0) {
            DEBUG("devmapper: delete device:%d err", device_id);
        }
        mark_device_id_free(devset, device_id);
        goto out;
    }

    if (close_transaction(devset) != 0) {
        ERROR("devmapper: close transaction failed, start to delete device with hash(%s)", hash);
        if (unregister_device(devset, hash) != 0) {
            DEBUG("devmapper: unregister device %s failed", hash);
        }

        if (dev_delete_device(pool_dev, device_id) != 0) {
            DEBUG("devmapper: delete device with hash:%s, device id:%d failed", hash, device_id);
        }
        mark_device_id_free(devset, device_id);
        ret = -1;
        goto out;
    }

out:
    free(pool_dev);
    if (ret != 0) {
        return NULL;
    }
    return info;
}

static int create_register_snap_device(struct device_set *devset, image_devmapper_device_info *base_info,
                                       const char *hash, uint64_t size)
{
    int ret = 0;
    int nret = 0;
    int device_id = 0;
    char *pool_dev = NULL;
    image_devmapper_device_info *info = NULL;

    if (get_next_free_device_id(devset, &device_id) != 0) {
        ERROR("devmapper: cannot get next free device id");
        ret = -1;
        goto out;
    }

    if (open_transaction(devset, hash, device_id) != 0) {
        ERROR("devmapper: Error opening transaction hash = %s deviceID = %d", hash, device_id);
        ret = -1;
        mark_device_id_free(devset, device_id);
        goto out;
    }

    pool_dev = get_pool_dev_name(devset);
    if (pool_dev == NULL) {
        ERROR("devmapper: get pool device name failed");
        ret = -1;
        goto out;
    }

    do {
        nret = dev_create_snap_device_raw(pool_dev, device_id, base_info->device_id);
        if (nret != 0) {
            ERROR("devmapper: create snap device with id:%d failed, err:%s", device_id, dev_strerror(nret));
            // Device ID already exists. This should not
            // happen. Now we have a mechanism to find
            // a free device ID. So something is not right.
            // Give a warning and continue.
            if (nret == ERR_DEVICE_ID_EXISTS) {
                if (get_next_free_device_id(devset, &device_id) != 0) {
                    ret = -1;
                    ERROR("devmapper: cannot get next free device id");
                    goto out;
                }

                if (refresh_transaction(devset, device_id) != 0) {
                    ret = -1;
                    ERROR("devmapper: Error refresh open transaction deviceID %s = %d", hash, device_id);
                    goto out;
                }
                continue;
            }
            ret = -1;
            DEBUG("devmapper: error creating snap device");
            mark_device_id_free(devset, device_id);
            goto out;
        }
        break;
    } while (true);

    info = register_device(devset, device_id, hash, size, devset->metadata_trans->open_transaction_id);
    if (info == NULL) {
        ERROR("devmapper: Error registering device");
        ret = -1;
        (void)dev_delete_device(pool_dev, device_id);
        mark_device_id_free(devset, device_id);
        goto out;
    }

    if (close_transaction(devset) != 0) {
        ERROR("devmapper: close transaction failed, start to delete device with hash(%s)", hash);
        ret = -1;
        (void)unregister_device(devset, hash);
        (void)dev_delete_device(pool_dev, device_id);
        mark_device_id_free(devset, device_id);
        goto out;
    }

out:
    free(pool_dev);
    return ret;
}

static int cancel_deferred_removal(struct device_set *devset, const char *hash)
{
    int i = 0;
    int ret = 0;
    int nret = 0;
    int retries = 100;
    char *dm_name = NULL;

    dm_name = get_dm_name(devset, hash);
    if (dm_name == NULL) {
        ERROR("devmapper: get dm device name with hash:%s failed", hash);
        ret = -1;
        goto out;
    }

    for (; i < retries; i++) {
        nret = dev_cancel_deferred_remove(dm_name);
        if (nret != 0) {
            if (nret == ERR_BUSY) {
                // If we see EBUSY it may be a transient error, sleep a bit and retry
                DEBUG("devmapper: cannot run canceling deferred remove task, device is busy, retry after 0.1 second");
                usleep(100000);
                continue;
            }
            ERROR("devmapper: cancel deferred remove for dm:%s failed, err:%s", dm_name, dev_strerror(nret));
            ret = -1;
        }
        goto out;
    }

out:
    free(dm_name);
    return ret;
}

static int take_snapshot(struct device_set *devset, const char *hash, image_devmapper_device_info *base_info,
                         uint64_t size)
{
    int ret = 0;
    int nret = 0;
    struct dm_info dinfo = { 0 };
    char *dm_name = NULL;
    bool resume_dev = false;
    bool deactive_dev = false;

    dm_name = get_dm_name(devset, base_info->hash);
    if (dm_name == NULL) {
        ERROR("demapper: get dm with id:%s name failed", base_info->hash);
        ret = -1;
        goto out;
    }

    if (pool_has_free_space(devset) != 0) {
        ERROR("devmapper: pool has no free space");
        ret = -1;
        goto out;
    }

    if (dev_get_info(&dinfo, dm_name) != 0) {
        ERROR("devmapper: get dev info with deferred failed");
        ret = -1;
        goto out;
    }

    if (dinfo.deferred_remove != 0) {
        nret = cancel_deferred_removal(devset, base_info->hash);
        if (nret != 0) {
            ERROR("devmapper: cancel deferred remove for device with hash:%s failed, err:%s", base_info->hash,
                  dev_strerror(nret));
            if (nret != ERR_ENXIO) {
                ERROR("devmapper: cancel device(id:%s) deferred remove failed", base_info->hash);
                ret = -1;
                goto out;
            }
            dinfo.exists = 0;
        } else {
            DEBUG("Start to deactive dev with hash:%s", base_info->hash);
            deactive_dev = true;
        }
    }

    if (dinfo.exists != 0) {
        DEBUG("devmapper: device:%s exists start to suspend before create snapshot", dm_name);
        if (dev_suspend_device(dm_name) != 0) {
            ERROR("devmapper: suspend dm with name:%s failed", dm_name);
            ret = -1;
            goto out;
        }
        resume_dev = true;
    }

    if (create_register_snap_device(devset, base_info, hash, size) != 0) {
        ERROR("devmapper: creat snap device from device %s failed", hash);
        ret = -1;
        goto out;
    }

out:
    if (deactive_dev) {
        (void)deactivate_device(devset, base_info);
    }

    if (resume_dev) {
        dev_resume_device(dm_name);
    }

    free(dm_name);
    return ret;
}

static int cancel_deferred_removal_if_needed(struct device_set *devset, image_devmapper_device_info *info)
{
    int ret = 0;
    int nret = 0;
    char *dm_name = NULL;
    struct dm_info dmi = { 0 };

    dm_name = get_dm_name(devset, info->hash);
    if (dm_name == NULL) {
        ERROR("devmapper: get dm device name with hash:%s failed", info->hash);
        ret = -1;
        goto out;
    }

    if (dev_get_info(&dmi, dm_name) != 0) {
        ERROR("devmapper: can not get info from dm %s", dm_name);
        ret = -1;
        goto out;
    }

    if (dmi.deferred_remove == 0) {
        DEBUG("Device:%s is already disabled deferred remove", dm_name);
        goto out;
    }

    nret = cancel_deferred_removal(devset, info->hash);
    if (nret != 0 && nret != ERR_BUSY) {
        ERROR("devmapper: cancel deferred remove for device with hash:%s failed, err:%s", info->hash,
              dev_strerror(nret));
        ret = -1;
        goto out;
    }

out:
    free(dm_name);
    return ret;
}

static int activate_device_if_needed(struct device_set *devset, image_devmapper_device_info *info, bool ignore_deleted)
{
    int ret = 0;
    struct dm_info dinfo = { 0 };
    char *dm_name = NULL;
    char *pool_dev_name = NULL;

    if (info->deleted && !ignore_deleted) {
        ERROR("devmapper: Can't activate device %s as it is marked for deletion", info->hash);
        return -1;
    }

    if (cancel_deferred_removal_if_needed(devset, info) != 0) {
        ERROR("devmapper: Device Deferred Removal Cancellation Failed");
        ret = -1;
        goto out;
    }

    dm_name = get_dm_name(devset, info->hash);
    if (dm_name == NULL) {
        ERROR("devmapper: get dm device name with hash:%s failed", info->hash);
        ret = -1;
        goto out;
    }

    if (dev_get_info(&dinfo, dm_name) != 0) {
        ERROR("devmapper: get device info failed");
        ret = -1;
        goto out;
    }

    if (dinfo.exists != 0) {
        DEBUG("device with name:%s already exists, no need to activate", dm_name);
        ret = 0;
        goto out;
    }

    pool_dev_name = get_pool_dev_name(devset);
    if (pool_dev_name == NULL) {
        ERROR("devmapper: get pool dev name failed");
        ret = -1;
        goto out;
    }

    if (dev_active_device(pool_dev_name, dm_name, info->device_id, info->size) != 0) {
        ERROR("devmapper: active device with hash:%d, id:%s, failed", info->device_id, info->hash);
        ret = -1;
        goto out;
    }

out:
    free(dm_name);
    free(pool_dev_name);
    return ret;
}

static int save_base_device_uuid(struct device_set *devset, image_devmapper_device_info *info)
{
    int ret = 0;
    char *base_dev_uuid = NULL;
    char *dev_fname = NULL;

    if (activate_device_if_needed(devset, info, false) != 0) {
        ERROR("devmapper: activate device %s failed", info->hash);
        ret = -1;
        goto free_out;
    }

    dev_fname = dev_name(devset, info);
    if (dev_fname == NULL) {
        ERROR("devmapper: get dm name failed");
        ret = -1;
        goto free_out;
    }

    base_dev_uuid = get_device_uuid(dev_fname);
    if (base_dev_uuid == NULL) {
        ERROR("devmapper: get base dev %s uuid failed", dev_fname);
        ret = -1;
        goto free_out;
    }

    devset->base_device_uuid = util_strdup_s(base_dev_uuid);
    if (save_deviceset_matadata(devset) != 0) {
        ERROR("devmapper: save deviceset metadata failed");
        ret = -1;
        goto free_out;
    }

free_out:
    deactivate_device(devset, info);
    free(dev_fname);
    free(base_dev_uuid);
    return ret;
}

static void run_mkfs_ext4(void *args)
{
    char **tmp_args = (char **)args;

    execvp(tmp_args[0], tmp_args);
}

static int save_base_device_filesystem(struct device_set *devset, const char *fs)
{
    free(devset->base_device_filesystem);
    devset->base_device_filesystem = util_strdup_s(fs);
    return save_deviceset_matadata(devset);
}

static int create_file_system(struct device_set *devset, image_devmapper_device_info *info)
{
#define ARGS_LEN 5
    int ret = 0;
    int i = 0;
    size_t cnt = 0;
    char *dev_fname = NULL;
    char **args = NULL;
    char *stdout_msg = NULL;
    char *stderr_msg = NULL;

    dev_fname = dev_name(devset, info);
    if (dev_fname == NULL) {
        ERROR("devmapper: get dev name failed");
        ret = -1;
        goto out;
    }

    if (!util_valid_str(devset->filesystem)) {
        free(devset->filesystem);
        devset->filesystem = util_strdup_s("ext4");
    }

    if (save_base_device_filesystem(devset, devset->filesystem) != 0) {
        ERROR("devmapper; save base device filesystem:%s failed", devset->filesystem);
        ret = -1;
        goto out;
    }

    if (strcmp(devset->filesystem, "ext4") != 0) {
        ERROR("devmapper: Unsupported filesystem type %s", devset->filesystem);
        ret = -1;
        goto out;
    }

    args = (char **)util_common_calloc_s(sizeof(char *) * (ARGS_LEN + devset->mkfs_args_len));
    if (args == NULL) {
        ERROR("devmapper: out of memory");
        ret = -1;
        goto out;
    }

    args[i++] = util_strdup_s("mkfs.ext4");
    args[i++] = util_strdup_s("-E");
    args[i++] = util_strdup_s("nodiscard,lazy_itable_init=0,lazy_journal_init=0");
    for (cnt = 0; cnt < devset->mkfs_args_len; cnt++) {
        args[i++] = util_strdup_s(devset->mkfs_args[cnt]);
    }
    args[i++] = util_strdup_s(dev_fname);

    if (!util_exec_cmd(run_mkfs_ext4, args, NULL, &stdout_msg, &stderr_msg)) {
        ERROR("Unexpected command output %s with error: %s", stdout_msg, stderr_msg);
        ret = -1;
        goto out;
    }

out:
    util_free_array(args);
    free(stdout_msg);
    free(stderr_msg);
    free(dev_fname);
    return ret;
}

static int create_base_image(struct device_set *devset)
{
    int ret = 0;
    image_devmapper_device_info *info = NULL;

    info = create_register_device(devset, "base");
    if (info == NULL) {
        ERROR("devmapper: create and register base device failed");
        ret = -1;
        goto out;
    }

    DEBUG("devmapper: Creating filesystem on base device-mapper thin volume");
    if (activate_device_if_needed(devset, info, false) != 0) {
        ERROR("devmapper: activate device %s failed", info->hash);
        ret = -1;
        goto out;
    }

    if (create_file_system(devset, info) != 0) {
        ERROR("devmapper: create file system for base dev failed");
        ret = -1;
        goto out;
    }
    info->initialized = true;

    if (save_metadata(devset, info) != 0) {
        ERROR("devmapper: save metadata for device %s failed", info->hash);
        ret = -1;
        info->initialized = false;
        goto out;
    }

    if (save_base_device_uuid(devset, info) != 0) {
        ERROR("devmapper: Could not query and save base device UUID");
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int check_thin_pool(struct device_set *devset)
{
    uint64_t total_size_in_sectors, transaction_id, data_used;
    uint64_t data_total, metadata_used, metadata_total;
    int ret = 0;

    if (pool_status(devset, &total_size_in_sectors, &transaction_id, &data_used, &data_total, &metadata_used,
                    &metadata_total) != 0) {
        ERROR("devmapper: get pool status failed");
        ret = -1;
        goto out;
    }

    if (data_used != 0) {
        ERROR("devmapper: Unable to take ownership of thin-pool (%s) that already has used data blocks",
              devset->thin_pool_device);
        ret = -1;
        goto out;
    }

    if (transaction_id != 0) {
        ERROR("devmapper: Unable to take ownership of thin-pool (%s) with non-zero transaction ID",
              devset->thin_pool_device);
        ret = -1;
        goto out;
    }

    DEBUG("devmapper:total_size_in_sectors:%lu, data_total:%lu, metadata_used:%lu, metadata_total:%lu",
          total_size_in_sectors, data_total, metadata_used, metadata_total);

out:
    return ret;
}

static int verify_base_device_uuidfs(struct device_set *devset, image_devmapper_device_info *base_info)
{
    int ret = 0;
    char *dev_fname = NULL;
    char *uuid = NULL;
    char *fs_type = NULL;

    if (activate_device_if_needed(devset, base_info, false) != 0) {
        ERROR("devmapper: activate device %s failed", base_info->hash);
        return -1;
    }

    dev_fname = dev_name(devset, base_info);
    if (dev_fname == NULL) {
        ERROR("devmapper: get dm name failed");
        ret = -1;
        goto out;
    }

    uuid = get_device_uuid(dev_fname);
    if (uuid == NULL) {
        ERROR("devmapper: get uuid err from device %s", dev_fname);
        ret = -1;
        goto out;
    }

    if (strcmp(devset->base_device_uuid, uuid) != 0) {
        ERROR("devmapper: Current Base Device UUID:%s does not match with stored UUID:%s. "
              "Possibly using a different thin pool than last invocation",
              uuid, devset->base_device_uuid);
        ret = -1;
        goto out;
    }

    if (!util_valid_str(devset->base_device_filesystem)) {
        // Now only support ext4, xfs and btrfs not support
        fs_type = util_strdup_s("ext4");
        if (fs_type == NULL) {
            ERROR("Dup filesystem:ext4 str failed");
            ret = -1;
            goto out;
        }

        if (save_base_device_filesystem(devset, fs_type) != 0) {
            ERROR("devmapper; save base device filesystem:%s failed", fs_type);
            ret = -1;
            goto out;
        }
    }

    if (strcasecmp(devset->base_device_filesystem, "ext4") != 0) {
        ERROR("devmapper:Current Base Device Filesystem:%s is not ext4, not surpport expect ext4",
              devset->base_device_filesystem);
        ret = -1;
        goto out;
    }

out:
    deactivate_device(devset, base_info);
    free(dev_fname);
    free(uuid);
    free(fs_type);
    return ret;
}

static int setup_verify_baseimages_uuidfs(struct device_set *devset, image_devmapper_device_info *base_info)
{
    int ret = 0;

    if (base_info == NULL) {
        ERROR("Invalid input param");
        return -1;
    }

    if (devset->base_device_uuid == NULL) {
        if (save_base_device_uuid(devset, base_info) != 0) {
            ERROR("devmapper: Could not query and save base device UUID");
            ret = -1;
        }
        goto out;
    }

    if (verify_base_device_uuidfs(devset, base_info) != 0) {
        ERROR("devmapper: Base Device UUID and Filesystem verification failed");
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static void append_mount_options(char **dest, const char *suffix)
{
    char *res_string = NULL;
    size_t length;

    if (*dest == NULL) {
        *dest = util_strdup_s(suffix);
        return;
    }

    if (suffix == NULL) {
        return;
    }

    if (strlen(suffix) > ((SIZE_MAX - strlen(*dest) - strlen(",")) - 1)) {
        ERROR("String is too long to be appended");
        return;
    }

    length = strlen(*dest) + strlen(",") + strlen(suffix) + 1;
    res_string = util_common_calloc_s(length);
    if (res_string == NULL) {
        ERROR("Out of memory");
        return;
    }
    (void)strcat(res_string, *dest);
    (void)strcat(res_string, ",");
    (void)strcat(res_string, suffix);

    free(*dest);
    *dest = res_string;
}

static int grow_fs(struct device_set *devset, image_devmapper_device_info *info)
{
#define FS_MOUNT_POINT "/run/containers/storage/mnt"
    int ret = 0;
    bool is_remove = false;
    char *mount_opt = NULL;
    char *dev_fname = NULL;

    if (activate_device_if_needed(devset, info, false) != 0) {
        ERROR("devmapper:error activating devmapper device %s", info->hash);
        ret = -1;
        goto out;
    }

    if (!util_dir_exists(FS_MOUNT_POINT)) {
        if (util_mkdir_p(FS_MOUNT_POINT, DEFAULT_DEVICE_SET_MODE) != 0) {
            ERROR("devmapper: mkdir %s failed", FS_MOUNT_POINT);
            ret = -1;
            goto out;
        }
        is_remove = true;
    }

    append_mount_options(&mount_opt, devset->mount_options);
    dev_fname = dev_name(devset, info);
    if (dev_fname == NULL) {
        ERROR("devmapper: get device:%s full name failed", info->hash);
        ret = -1;
        goto out;
    }

    if (util_mount(dev_fname, FS_MOUNT_POINT, devset->base_device_filesystem, mount_opt) != 0) {
        ERROR("Error mounting '%s' on '%s' ", dev_fname, FS_MOUNT_POINT);
        ret = -1;
        goto out;
    }

    if (strcmp(devset->base_device_filesystem, "ext4") == 0) {
        if (exec_grow_fs_command("resize2fs", dev_fname) != 0) {
            ERROR("Failed execute resize2fs to grow rootfs");
            ret = -1;
            goto clean_mount;
        }
    } else {
        ERROR("Unsupported filesystem type %s", devset->base_device_filesystem);
        ret = -1;
        goto clean_mount;
    }

clean_mount:
    if (umount2(FS_MOUNT_POINT, MNT_DETACH) < 0 && errno != EINVAL) {
        WARN("Failed to umount directory %s:%s", FS_MOUNT_POINT, strerror(errno));
    }

out:
    deactivate_device(devset, info);
    if (is_remove && util_path_remove(FS_MOUNT_POINT) != 0) {
        WARN("devmapper: remove path:%s failed", FS_MOUNT_POINT);
    }
    free(dev_fname);
    free(mount_opt);
    return ret;
}

static int check_grow_base_device_fs(struct device_set *devset, image_devmapper_device_info *base_info)
{
    uint64_t base_dev_size;

    if (!devset->user_base_size) {
        return 0;
    }

    base_dev_size = get_base_device_size(devset);
    if (devset->base_fs_size < base_dev_size) {
        ERROR("devmapper: Base fs size:%lu cannot be smaller than %lu", devset->base_fs_size, base_dev_size);
        return -1;
    }

    if (devset->base_fs_size == base_dev_size) {
        return 0;
    }

    base_info->size = devset->base_fs_size;

    if (save_metadata(devset, base_info) != 0) {
        ERROR("devmapper: save device with hash:%s metadata failed", base_info->hash);
        if (!metadata_store_remove(base_info->hash, devset->meta_store)) {
            ERROR("devmapper: remove unused device from store failed");
        }
        return -1;
    }

    return grow_fs(devset, base_info);
}

static int mark_for_deferred_deletion(struct device_set *devset, image_devmapper_device_info *info)
{
    if (info->deleted) {
        return 0;
    }

    info->deleted = true;
    if (save_metadata(devset, info) != 0) {
        info->deleted = false;
        return -1;
    }
    devset->nr_deleted_devices++;

    return 0;
}

static int delete_transaction(struct device_set *devset, image_devmapper_device_info *info, bool sync_delete)
{
    int ret = 0;
    int nret = 0;
    char *pool_fname = NULL;

    if (open_transaction(devset, info->hash, info->device_id) != 0) {
        ERROR("devmapper: Error opening transaction hash=%s, device id=%d", info->hash, info->device_id);
        return -1;
    }

    pool_fname = get_pool_dev_name(devset);
    nret = dev_delete_device(pool_fname, info->device_id);
    if (nret != 0) {
        ERROR("devmapper: delete device directly with hash:%s, err:%s", info->hash, dev_strerror(nret));
        if (sync_delete || nret != ERR_BUSY) {
            ERROR("devmapper: Error deleting device");
            ret = -1;
            goto out;
        }
    }

    if (nret == 0) {
        DEBUG("devmapper: delete device with hash(%s) success", info->hash);
        if (unregister_device(devset, info->hash) != 0) {
            ERROR("devmapper: unregiste device:%s failed", info->hash);
            ret = -1;
            goto out;
        }
        if (info->deleted) {
            devset->nr_deleted_devices--;
        }
        mark_device_id_free(devset, info->device_id);
    } else {
        ERROR("devmapper: delete device directly with hash(%s) failed, start to mark deferred deletion", info->hash);
        if (mark_for_deferred_deletion(devset, info) != 0) {
            ERROR("devmapper: mark device with hash:%s deferred deletion failed", info->hash);
            ret = -1;
            goto out;
        }
    }

out:
    (void)close_transaction(devset);
    free(pool_fname);
    return ret;
}

static int do_delete_device(struct device_set *devset, const char *hash, bool sync_delete)
{
    int ret = 0;
    devmapper_device_info_t *device_info = NULL;

    if (devset == NULL || hash == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    device_info = lookup_device(devset, hash);
    if (device_info == NULL) {
        ERROR("Delete device error with lookuping device with hash(%s) failed", hash);
        return -1;
    }

    if (deactivate_device_mode(devset, device_info->info) != 0) {
        ERROR("devmapper: Error deactivating device");
        ret = -1;
        goto out;
    }

    if (delete_transaction(devset, device_info->info, sync_delete) != 0) {
        ERROR("devmapper: delete transaction failed");
        ret = -1;
        goto out;
    }

out:
    devmapper_device_info_ref_dec(device_info);
    return ret;
}

static int setup_base_image(struct device_set *devset)
{
    int ret = 0;
    devmapper_device_info_t *device_info = NULL;

    device_info = lookup_device(devset, "base");

    // base image already exists. If it is initialized properly, do UUID
    // verification and return. Otherwise remove image and set it up
    // fresh.
    if (device_info != NULL) {
        DEBUG("devmapper: base device is not NULL, start to verify and try growing fs size");
        if (device_info->info->initialized && !device_info->info->deleted) {
            if (setup_verify_baseimages_uuidfs(devset, device_info->info) != 0) {
                ERROR("devmapper: do base image uuid verification failed");
                ret = -1;
                goto out;
            }

            if (check_grow_base_device_fs(devset, device_info->info) != 0) {
                ERROR("devmapper: grow base device fs failed");
                ret = -1;
            }
            goto out;
        }

        DEBUG("devmapper: removing uninitialized base image");

        if (do_delete_device(devset, "base", true) != 0) {
            ERROR("devmapper: remove uninitialized base image failed");
            ret = -1;
            goto out;
        }
    }

    // If we are setting up base image for the first time, make sure
    // thin pool is empty.
    if (util_valid_str(devset->thin_pool_device) && device_info == NULL) {
        DEBUG("Start to check thin pool");
        if (check_thin_pool(devset) != 0) {
            ERROR("devmapper: check thin pool failed");
            ret = -1;
            goto out;
        }
    }

    if (create_base_image(devset) != 0) {
        ERROR("devmapper: create base image failed");
        ret = -1;
        goto out;
    }

out:
    devmapper_device_info_ref_dec(device_info);
    return ret;
}

static int do_get_devset_device_prefix(struct device_set *devset)
{
    int ret = 0;
    int nret = 0;
    struct stat st;
    char prefix[PATH_MAX] = { 0 };

    nret = stat(devset->root, &st);
    if (nret < 0) {
        ERROR("devmapper: Error looking up dir %s", devset->root);
        ret = -1;
        goto out;
    }
    nret = snprintf(prefix, sizeof(prefix), "container-%u:%u-%u", major(st.st_dev), minor(st.st_dev),
                    (unsigned int)st.st_ino);
    if (nret < 0 || (size_t)nret >= sizeof(prefix)) {
        ERROR("Failed to sprintf device prefix");
        ret = -1;
        goto out;
    }
    devset->device_prefix = util_strdup_s(prefix);

out:
    return ret;
}

static int do_check_all_devices(struct device_set *devset)
{
    int ret = 0;
    char **devices_list = NULL;
    size_t devices_len = 0;
    size_t i = 0;
    uint64_t start, length;
    char *target_type = NULL;
    char *params = NULL;
    char device_path[PATH_MAX] = { 0 };
    struct stat st;
    int nret = 0;

    // Equal to  "dmsetup ls" .  That is to say, devices_len is not zero, because isulad-thinpool exists.
    if (dev_get_device_list(&devices_list, &devices_len) != 0) {
        ERROR("devicemapper: failed to get device list");
        ret = -1;
        goto out;
    }

    for (i = 0; i < devices_len; i++) {
        if (!util_has_prefix(devices_list[i], devset->device_prefix)) {
            continue;
        }
        UTIL_FREE_AND_SET_NULL(target_type);
        UTIL_FREE_AND_SET_NULL(params);
        if (dev_get_status(&start, &length, &target_type, &params, devices_list[i]) != 0) {
            WARN("devmapper: get device status %s failed", devices_list[i]);
            continue;
        }
        // remove broken device
        if (length == 0) {
            nret = dev_delete_device_force(devices_list[i]);
            if (nret != 0) {
                WARN("devmapper: remove broken device %s failed, err:%s", devices_list[i], dev_strerror(nret));
            }
            DEBUG("devmapper: remove broken device: %s", devices_list[i]);
        }
        (void)memset(device_path, 0, sizeof(device_path));
        nret = snprintf(device_path, sizeof(device_path), "/dev/mapper/%s", devices_list[i]);
        if (nret < 0 || (size_t)nret >= sizeof(device_path)) {
            ERROR("Failed to snprintf device path");
            continue;
        }
        if (stat(device_path, &st)) {
            nret = dev_delete_device_force(devices_list[i]);
            if (nret != 0) {
                WARN("devmapper: remove incompelete device %s, err:%s", devices_list[i], dev_strerror(nret));
            }
            DEBUG("devmapper: remove incompelete device: %s", devices_list[i]);
        }
    }

out:
    util_free_array_by_len(devices_list, devices_len);
    free(target_type);
    free(params);
    return ret;
}

static int do_init_metadate(struct device_set *devset)
{
    int ret = 0;
    bool pool_exist = false;
    char *pool_name = NULL;

    pool_name = util_strdup_s(devset->thin_pool_device);
    if (pool_name == NULL) {
        ERROR("devmapper: pool name is null");
        ret = -1;
        goto out;
    }

    pool_exist = thin_pool_exists(devset, pool_name);
    if (!pool_exist || !util_valid_str(devset->thin_pool_device)) {
        ERROR("devmapper: thin pool is not exist or caller did not pass us a pool, please create it firstly");
        ret = -1;
        goto out;
    }

    if (init_metadata(devset, pool_name) != 0) {
        ERROR("devmapper: init metadata failed");
        ret = -1;
        goto out;
    }

out:
    free(pool_name);
    return ret;
}

static int do_devmapper_init(struct device_set *devset)
{
    int ret = 0;
    bool support = false;
    char *metadata_path = NULL;

    support = udev_set_sync_support(true);
    if (!support) {
        ERROR("devmapper: Udev sync is not supported. This will lead to data loss and unexpected behavior.");
        if (!devset->override_udev_sync_check) {
            ERROR("devmapper: driver do not support udev sync");
            ret = -1;
            goto out;
        }
    }

    metadata_path = metadata_dir(devset);
    if (util_mkdir_p(metadata_path, DEFAULT_DEVICE_SET_MODE) != 0) {
        ERROR("mkdir path %s failed", metadata_path);
        ret = -1;
        goto out;
    }

    if (do_get_devset_device_prefix(devset) != 0) {
        ERROR("Failed to get devset prefix");
        ret = -1;
        goto out;
    }

    // If checking failed, we just print a log, there is no need to process the error that do not affect isulad starting
    if (do_check_all_devices(devset) != 0) {
        ERROR("Failed to check all devset devices");
    }

    if (do_init_metadate(devset) != 0) {
        ERROR("devmapper: init metadata failed");
        ret = -1;
        goto out;
    }

    if (load_deviceset_metadata(devset) != 0) {
        ERROR("devmapper: load device set metadata failed");
        ret = -1;
        goto out;
    }

    if (setup_base_image(devset) != 0) {
        ERROR("devmapper: setup base image failed");
        ret = -1;
        goto out;
    }

out:
    free(metadata_path);
    return ret;
}

static void device_id_map_kvfree(void *key, void *value)
{
    free(key);
    free(value);
}

static int determine_driver_capabilities(const char *version, struct device_set *devset)
{
    int ret = 0;
    int64_t major, minor;
    char **tmp_str = NULL;
    size_t tmp_str_len = 0;

    tmp_str = util_string_split(version, '.');
    if (tmp_str == NULL) {
        ERROR("Cannot split version:%s", version);
        ret = -1;
        goto out;
    }
    tmp_str_len = util_array_len((const char **)tmp_str);
    if (tmp_str_len < 2) {
        ERROR("devmapper: driver version:%s format error", version);
        ret = -1;
        goto out;
    }

    ret = util_parse_byte_size_string(tmp_str[0], &major);
    if (ret != 0) {
        ERROR("devmapper: invalid size: '%s': %s", tmp_str[0], strerror(-ret));
        ret = -1;
        goto out;
    }

    if (major < 4) {
        ERROR("devicamapper driver version:(%ld.xxx) < 4.27.0, do not surpport deferred removal", major);
        isulad_set_error_message("devicamapper driver version:(%ld.xxx) < 4.27.0, do not surpport deferred removal",
                                 major);
        ret = -1;
        goto out;
    }

    if (major > 4) {
        DEBUG("devicemapper driver version >= 4.27.0, surpport deferred removal");
        goto out;
    }

    ret = util_parse_byte_size_string(tmp_str[1], &minor);
    if (ret != 0) {
        ERROR("devmapper: invalid size: '%s': %s", tmp_str[1], strerror(-ret));
        ret = -1;
        goto out;
    }
    /*
     * If major is 4 and minor is 27, then there is no need to
     * check for patch level as it can not be less than 0.
     */
    if (minor < 27) {
        ERROR("devicamapper driver version (4.%ld) < 4.27.0, , do not surpport deferred removal", minor);
        isulad_set_error_message("devicamapper driver version (4.%ld) < 4.27.0, , do not surpport deferred removal",
                                 minor);
        ret = -1;
        goto out;
    }

out:
    util_free_array(tmp_str);
    return ret;
}

static int devmapper_init_cap_by_version(struct device_set *devset)
{
    int ret = 0;
    char *version = NULL;

    version = dev_get_driver_version();
    if (version == NULL) {
        ERROR("devmapper: driver not supported");
        ret = -1;
        goto out;
    }

    if (determine_driver_capabilities(version, devset) != 0) {
        ERROR("devmapper: determine driver capabilities failed");
        ret = -1;
        goto out;
    }

out:
    free(version);
    return ret;
}

static int devmapper_init_devset(const char *driver_home, const char **options, size_t len, struct graphdriver *driver)
{
    int ret = 0;
    struct device_set *devset = NULL;

    devset = util_common_calloc_s(sizeof(struct device_set));
    if (devset == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    driver->devset = devset;

    devset->root = util_strdup_s(driver_home);
    devset->user_base_size = false;
    devset->base_fs_size = 10 * SIZE_GB;
    devset->filesystem = util_strdup_s("ext4");
    devset->mkfs_args = NULL;
    devset->mkfs_args_len = 0;
    devset->mount_options = NULL;
    devset->override_udev_sync_check = DEFAULT_UDEV_SYNC_OVERRIDE;
    devset->do_blk_discard = false;
    devset->thinp_block_size = DEFAULT_THIN_BLOCK_SIZE;
    devset->min_free_space_percent = DEFAULT_MIN_FREE_SPACE_PERCENT;
    devset->device_id_map = map_new(MAP_INT_INT, NULL, device_id_map_kvfree);
    if (devset->device_id_map == NULL) {
        ERROR("devmapper: failed to allocate device id map");
        ret = -1;
        goto out;
    }
    devset->udev_wait_timeout = DEFAULT_UDEV_WAITTIMEOUT;
    devset->metadata_trans = util_common_calloc_s(sizeof(image_devmapper_transaction));
    if (devset->metadata_trans == NULL) {
        ERROR("Memory out");
        ret = -1;
        goto out;
    }

    if (devmapper_parse_options(devset, options, len) != 0) {
        ERROR("devmapper: parse options failed");
        ret = -1;
        goto out;
    }

    if (devmapper_init_cap_by_version(devset) != 0) {
        ERROR("failed to init devmapper cap");
        ret = -1;
        goto out;
    }

    devset->meta_store = metadata_store_new();
    if (devset->meta_store == NULL) {
        ERROR("Failed to init metadata store");
        ret = -1;
        goto out;
    }

    if (pthread_rwlock_init(&devset->devmapper_driver_rwlock, NULL) != 0) {
        ERROR("Failed to init devmapper conf rwlock");
        ret = -1;
        goto out;
    }

out:
    return ret;
}

int device_set_init(struct graphdriver *driver, const char *driver_home, const char **options, size_t len)
{
    int ret = 0;

    if (driver == NULL || driver_home == NULL || options == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    log_with_errno_init();

    if (devmapper_init_devset(driver_home, options, len, driver) != 0) {
        ERROR("Failed to init devset");
        ret = -1;
        goto out;
    }

    if (set_dev_dir(DEVICE_DIRECTORY) != 0) {
        ERROR("devmapper: set dev dir /dev failed");
        ret = -1;
        goto out;
    }

    set_udev_wait_timeout(driver->devset->udev_wait_timeout);
    if (do_devmapper_init(driver->devset) != 0) {
        ERROR("Fail to do devmapper init");
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int parse_storage_opt(const json_map_string_string *opts, uint64_t *size)
{
    int ret = 0;
    size_t i = 0;

    if (size == NULL) {
        ERROR("Invalid param, size is null");
        return -1;
    }

    if (opts == NULL || opts->len == 0) {
        goto out;
    }

    for (i = 0; i < opts->len; i++) {
        if (strcasecmp("size", opts->keys[i]) == 0) {
            int64_t converted = 0;

            ret = util_parse_byte_size_string(opts->values[i], &converted);
            if (ret != 0) {
                ERROR("Invalid size: '%s': %s", opts->values[i], strerror(-ret));
                ret = -1;
                goto out;
            }
            *size = (uint64_t)converted;
            break;
        } else {
            ERROR("Unknown option %s", opts->keys[i]);
            ret = -1;
            goto out;
        }
    }

out:
    return ret;
}

static int grow_device_fs(struct device_set *devset, const char *hash, uint64_t size, uint64_t base_size)
{
    int ret = 0;
    devmapper_device_info_t *device_info = NULL;

    if (size <= base_size) {
        return 0;
    } else {
        DEBUG("devmapper: new fs size is larger than old basesize, start to grow fs");
        device_info = lookup_device(devset, hash);
        if (device_info == NULL) {
            ERROR("devmapper: lookup device %s failed", hash);
            ret = -1;
            goto out;
        }

        if (grow_fs(devset, device_info->info) != 0) {
            ret = -1;
            goto out;
        }
    }
out:
    return ret;
}

int add_device(const char *hash, const char *base_hash, struct device_set *devset,
               const json_map_string_string *storage_opts)
{
    int ret = 0;
    devmapper_device_info_t *base_device_info = NULL;
    devmapper_device_info_t *device_info = NULL;
    uint64_t size = 0;

    if (devset == NULL || hash == NULL) {
        ERROR("devmapper: invalid input params to add device");
        return -1;
    }

    if (pthread_rwlock_wrlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("lock devmapper conf failed");
        return -1;
    }

    base_device_info = lookup_device(devset, util_valid_str(base_hash) ? base_hash : "base");
    if (base_device_info == NULL) {
        ERROR("Lookup device %s failed, not found", util_valid_str(base_hash) ? base_hash : "base");
        ret = -1;
        goto free_out;
    }

    if (base_device_info->info->deleted) {
        ERROR("devmapper: Base device %s has been marked for deferred deletion", base_device_info->info->hash);
        ret = -1;
        goto free_out;
    }

    device_info = lookup_device(devset, hash);
    if (device_info != NULL) {
        ERROR("devmapper: device %s already exists", hash);
        ret = -1;
        goto free_out;
    }

    if (parse_storage_opt(storage_opts, &size) != 0) {
        ERROR("devmapper: parse storage opts for adding device failed");
        ret = -1;
        goto free_out;
    }

    if (size == 0) {
        size = base_device_info->info->size;
    }

    if (size < base_device_info->info->size) {
        ERROR("devmapper: Container size:%lu cannot be smaller than %lu", size, base_device_info->info->size);
        isulad_set_error_message("Container size cannot be smaller than %lu", base_device_info->info->size);
        ret = -1;
        goto free_out;
    }

    if (take_snapshot(devset, hash, base_device_info->info, size) != 0) {
        ret = -1;
        goto free_out;
    }

    if (grow_device_fs(devset, hash, size, base_device_info->info->size) != 0) {
        ERROR("Grow new deivce fs failed");
        // Here, we need to delete device directly instead of deferred deleting, so that we can retry to add device with the same hash successfully.
        if (do_delete_device(devset, hash, true) != 0) {
            ERROR("devmapper: remove new snapshot device failed");
        }
        ret = -1;
        goto free_out;
    }

free_out:
    devmapper_device_info_ref_dec(base_device_info);
    devmapper_device_info_ref_dec(device_info);
    if (pthread_rwlock_unlock(&devset->devmapper_driver_rwlock)) {
        ERROR("unlock devmapper conf failed");
    }
    return ret;
}

static char *generate_mount_options(const struct driver_mount_opts *moptions, const char *dev_options)
{
    char *res_str = NULL;

    append_mount_options(&res_str, dev_options);
#ifdef ENABLE_SELINUX
    if (moptions != NULL && moptions->mount_label != NULL) {
        char *tmp = NULL;

        tmp = selinux_format_mountlabel(res_str, moptions->mount_label);
        if (tmp == NULL) {
            goto error_out;
        }
        free(res_str);
        res_str = tmp;
        tmp = NULL;
    }
    goto out;

error_out:
    free(res_str);
    res_str = NULL;

out:
#endif
    return res_str;
}

int mount_device(const char *hash, const char *path, const struct driver_mount_opts *mount_opts,
                 struct device_set *devset)
{
    int ret = 0;
    devmapper_device_info_t *device_info = NULL;
    char *dev_fname = NULL;
    char *options = NULL;

    if (hash == NULL || path == NULL) {
        ERROR("devmapper: invalid input params to mount device");
        return -1;
    }

    if (pthread_rwlock_wrlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("lock devmapper conf failed");
        return -1;
    }

    device_info = lookup_device(devset, hash);
    if (device_info == NULL) {
        ERROR("devmapper: lookup device:\"%s\" failed", hash);
        ret = -1;
        goto free_out;
    }

    if (device_info->info->deleted) {
        ERROR("devmapper: Base device %s has been marked for deferred deletion", device_info->info->hash);
        ret = -1;
        goto free_out;
    }
    dev_fname = dev_name(devset, device_info->info);
    if (dev_fname == NULL) {
        ERROR("devmapper: failed to get device full name");
        ret = -1;
        goto free_out;
    }

    if (activate_device_if_needed(devset, device_info->info, false) != 0) {
        ERROR("devmapper: Error activating devmapper device for \"%s\"", hash);
        ret = -1;
        goto free_out;
    }

    options = generate_mount_options(mount_opts, devset->mount_options);
    if (util_mount(dev_fname, path, "ext4", options) != 0) {
        ERROR("devmapper: Error mounting %s on %s", dev_fname, path);
        ret = -1;
        goto free_out;
    }

free_out:
    devmapper_device_info_ref_dec(device_info);
    if (pthread_rwlock_unlock(&devset->devmapper_driver_rwlock)) {
        ERROR("unlock devmapper conf failed");
        ret = -1;
    }
    free(dev_fname);
    free(options);
    return ret;
}

// UnmountDevice unmounts the device and removes it from hash.
int unmount_device(const char *hash, const char *mount_path, struct device_set *devset)
{
    int ret = 0;
    devmapper_device_info_t *device_info = NULL;

    if (hash == NULL || mount_path == NULL) {
        ERROR("devmapper: invalid input params to unmount device");
        return -1;
    }

    if (pthread_rwlock_wrlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("lock devmapper conf failed");
        return -1;
    }

    device_info = lookup_device(devset, hash);
    if (device_info == NULL) {
        ERROR("devmapper: lookup device: \"%s\" failed", hash);
        ret = -1;
        goto free_out;
    }

    if (umount2(mount_path, MNT_DETACH) < 0 && errno != EINVAL) {
        ERROR("Failed to umount directory %s:%s", mount_path, strerror(errno));
        ret = -1;
        goto free_out;
    }

    if (deactivate_device(devset, device_info->info) != 0) {
        ERROR("devmapper: Error deactivating device");
        ret = -1;
        goto free_out;
    }

free_out:
    devmapper_device_info_ref_dec(device_info);
    if (pthread_rwlock_unlock(&devset->devmapper_driver_rwlock)) {
        ERROR("unlock devmapper conf failed");
        ret = -1;
    }
    return ret;
}

bool has_device(const char *hash, struct device_set *devset)
{
    bool res = false;
    devmapper_device_info_t *device_info = NULL;

    if (!util_valid_str(hash) || devset == NULL) {
        ERROR("devmapper: invalid input params to judge device metadata exists");
        return false;
    }

    if (pthread_rwlock_wrlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("lock devmapper conf failed");
        return false;
    }

    device_info = lookup_device(devset, hash);
    if (device_info == NULL) {
        ERROR("devmapper: lookup device: \"%s\" failed", hash);
        goto free_out;
    }

    res = true;

free_out:
    devmapper_device_info_ref_dec(device_info);
    (void)pthread_rwlock_unlock(&devset->devmapper_driver_rwlock);
    return res;
}

int delete_device(const char *hash, bool sync_delete, struct device_set *devset)
{
    int ret = 0;

    if (devset == NULL || hash == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    if (pthread_rwlock_wrlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("lock devmapper conf failed");
        return -1;
    }

    if (do_delete_device(devset, hash, sync_delete) != 0) {
        ERROR("devmapper: do delete device: \"%s\" failed", hash);
        ret = -1;
        goto free_out;
    }

free_out:
    if (pthread_rwlock_unlock(&devset->devmapper_driver_rwlock)) {
        ERROR("unlock devmapper conf failed");
        ret = -1;
    }
    return ret;
}

int export_device_metadata(struct device_metadata *dev_metadata, const char *hash, struct device_set *devset)
{
    int ret = 0;
    char *dm_name = NULL;
    devmapper_device_info_t *device_info = NULL;

    if (hash == NULL || dev_metadata == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    if (pthread_rwlock_wrlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("lock devmapper conf failed");
        return -1;
    }

    dm_name = get_dm_name(devset, hash);
    if (dm_name == NULL) {
        ERROR("devmapper: failed to get device: \"%s\" dm name", hash);
        ret = -1;
        goto free_out;
    }

    device_info = lookup_device(devset, hash);
    if (device_info == NULL) {
        ERROR("devmapper: lookup device: \"%s\" failed", hash);
        ret = -1;
        goto free_out;
    }

    dev_metadata->device_id = device_info->info->device_id;
    dev_metadata->device_size = device_info->info->size;
    dev_metadata->device_name = util_strdup_s(dm_name);

free_out:
    if (pthread_rwlock_unlock(&devset->devmapper_driver_rwlock)) {
        ERROR("unlock devmapper conf failed");
        ret = -1;
    }
    free(dm_name);
    devmapper_device_info_ref_dec(device_info);
    return ret;
}

void free_devmapper_status(struct status *st)
{
    if (st == NULL) {
        return;
    }

    UTIL_FREE_AND_SET_NULL(st->pool_name);
    UTIL_FREE_AND_SET_NULL(st->data_file);
    UTIL_FREE_AND_SET_NULL(st->metadata_file);
    UTIL_FREE_AND_SET_NULL(st->base_device_fs);
    UTIL_FREE_AND_SET_NULL(st->library_version);
    UTIL_FREE_AND_SET_NULL(st->sem_msg);

    free(st);
}

struct status *device_set_status(struct device_set *devset)
{
    struct status *st = NULL;
    uint64_t total_size_in_sectors, transaction_id, data_used;
    uint64_t data_total, metadata_used, metadata_total;
    uint64_t min_free_data;
    int sem_usz = 0;
    int sem_mni = 0;

    if (pthread_rwlock_wrlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("lock devmapper conf failed");
        return NULL;
    }

    st = util_common_calloc_s(sizeof(struct status));
    if (st == NULL) {
        ERROR("devmapper: out of memory");
        goto free_out;
    }

    st->pool_name = util_strdup_s(devset->thin_pool_device);
    st->data_file = util_strdup_s(devset->data_device);
    st->metadata_file = util_strdup_s(devset->metadata_device);
    st->udev_sync_supported = udev_sync_supported();
    st->deferred_remove_enabled = true;
    st->deferred_delete_enabled = true;
    st->deferred_deleted_device_count = devset->nr_deleted_devices;
    st->base_device_size = get_base_device_size(devset);
    st->base_device_fs = util_strdup_s(devset->base_device_filesystem);
    st->library_version = dev_get_library_version();
    st->sem_msg = NULL;

    if (pool_status(devset, &total_size_in_sectors, &transaction_id, &data_used, &data_total, &metadata_used,
                    &metadata_total) == 0) {
        if (data_total == 0) {
            ERROR("devmapper: device data total value is zero");
            free_devmapper_status(st);
            st = NULL;
            goto free_out;
        }
        uint64_t block_size_in_sectors = total_size_in_sectors / data_total;
        st->data.used = data_used * block_size_in_sectors * 512;
        st->data.total = data_total * block_size_in_sectors * 512;
        st->data.available = st->data.total - st->data.used;

        st->metadata.used = metadata_used * 4096;
        st->metadata.total = metadata_total * 4096;
        st->metadata.available = st->metadata.total - st->metadata.used;

        st->sector_size = block_size_in_sectors * 512;

        min_free_data = (data_total * (uint64_t)devset->min_free_space_percent) / 100;
        st->min_free_space = min_free_data * block_size_in_sectors * 512;
    }
    dev_check_sem_set_stat(&sem_usz, &sem_mni);
    st->semusz = sem_usz;
    st->semmni = sem_mni;
    if (sem_usz == sem_mni) {
        int msg_len = 0;
        char msg[PATH_MAX] = { 0 };

        msg_len = snprintf(msg, PATH_MAX, "system semaphore nums has attached limit: %d", sem_usz);
        if (msg_len < 0 || msg_len >= PATH_MAX) {
            ERROR("Cannot get semaphore err msg");
            free_devmapper_status(st);
            st = NULL;
            goto free_out;
        }
        st->sem_msg = util_strdup_s(msg);
    }

free_out:
    (void)pthread_rwlock_unlock(&devset->devmapper_driver_rwlock);
    return st;
}

static int umount_deactivate_dev_all(struct device_set *devset)
{
    int ret = 0;
    DIR *dp = NULL;
    struct dirent *entry = NULL;
    char fname[PATH_MAX] = { 0 };
    devmapper_device_info_t *device_info = NULL;
    char *mnt_root = NULL;

    mnt_root = util_path_join(devset->root, "mnt");
    if (mnt_root == NULL) {
        ERROR("devmapper:join path %s/mnt failed", devset->root);
        ret = -1;
        goto out;
    }

    dp = opendir(mnt_root);
    if (dp == NULL) {
        ERROR("devmapper: open dir %s failed", mnt_root);
        ret = -1;
        goto out;
    }

    // Do my best to umount all of the device that has been mounted
    while ((entry = readdir(dp)) != NULL) {
        struct stat st;
        int pathname_len;

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        (void)memset(fname, 0, sizeof(fname));
        pathname_len = snprintf(fname, PATH_MAX, "%s/%s", mnt_root, entry->d_name);
        if (pathname_len < 0 || pathname_len >= PATH_MAX) {
            ERROR("Pathname too long");
            continue;
        }

        if (stat(fname, &st) != 0) {
            ERROR("devmapper: get %s stat error:%s", fname, strerror(errno));
            continue;
        }

        if (!S_ISDIR(st.st_mode)) {
            DEBUG("devmapper: skipping regular file just to process dir");
            continue;
        }

        if (umount2(fname, MNT_DETACH) < 0 && errno != EINVAL) {
            ERROR("Failed to umount directory %s:%s", fname, strerror(errno));
        }

        device_info = lookup_device(devset, entry->d_name);
        if (device_info == NULL) {
            DEBUG("devmapper: shutdown lookup device %s err", entry->d_name);
        } else if (deactivate_device(devset, device_info->info) != 0) {
            DEBUG("devmapper: shutdown deactivate device %s err", entry->d_name);
        }
        devmapper_device_info_ref_dec(device_info);
    }

    device_info = lookup_device(devset, "base");
    if (device_info != NULL) {
        if (deactivate_device(devset, device_info->info) != 0) {
            DEBUG("devmapper: shutdown deactivate base device err");
        }
        devmapper_device_info_ref_dec(device_info);
    }

out:
    closedir(dp);
    free(mnt_root);
    return ret;
}

int device_set_shutdown(struct device_set *devset, const char *home)
{
    int ret = 0;

    if (devset == NULL || home == NULL) {
        ERROR("Invalid input params to shutdown device set");
        return -1;
    }

    if (pthread_rwlock_wrlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("lock devmapper conf failed");
        return -1;
    }

    if (save_deviceset_matadata(devset)) {
        DEBUG("devmapper: save deviceset metadata failed");
    }

    if (umount_deactivate_dev_all(devset) != 0) {
        ERROR("devmapper: Shutdown umount device failed");
        ret = -1;
        goto free_out;
    }

free_out:
    if (pthread_rwlock_unlock(&(devset->devmapper_driver_rwlock)) != 0) {
        ERROR("unlock devmapper conf failed");
        return -1;
    }
    return ret;
}

void free_device_set(struct device_set *devset)
{
    if (devset == NULL) {
        return;
    }

    UTIL_FREE_AND_SET_NULL(devset->root);
    UTIL_FREE_AND_SET_NULL(devset->device_prefix);
    metadata_store_free(devset->meta_store);
    devset->meta_store = NULL;
    map_free(devset->device_id_map);
    devset->device_id_map = NULL;
    UTIL_FREE_AND_SET_NULL(devset->filesystem);
    pthread_rwlock_destroy(&(devset->devmapper_driver_rwlock));
    UTIL_FREE_AND_SET_NULL(devset->mount_options);
    util_free_array_by_len(devset->mkfs_args, devset->mkfs_args_len);
    UTIL_FREE_AND_SET_NULL(devset->data_device);
    UTIL_FREE_AND_SET_NULL(devset->metadata_device);
    UTIL_FREE_AND_SET_NULL(devset->thin_pool_device);
    free_image_devmapper_transaction(devset->metadata_trans);
    devset->metadata_trans = NULL;
    UTIL_FREE_AND_SET_NULL(devset->base_device_uuid);
    UTIL_FREE_AND_SET_NULL(devset->base_device_filesystem);

    free(devset);
}
