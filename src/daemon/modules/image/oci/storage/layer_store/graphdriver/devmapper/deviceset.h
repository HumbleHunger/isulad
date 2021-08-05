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
* Description: provide devicemapper graphdriver function definition
******************************************************************************/
#ifndef DAEMON_MODULES_IMAGE_OCI_STORAGE_LAYER_STORE_GRAPHDRIVER_DEVMAPPER_DEVICESET_H
#define DAEMON_MODULES_IMAGE_OCI_STORAGE_LAYER_STORE_GRAPHDRIVER_DEVMAPPER_DEVICESET_H

#include <pthread.h>
#include <isula_libutils/json_common.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver.h"
#include "metadata_store.h"

struct device_set;
struct driver_mount_opts;
struct graphdriver;

#ifdef __cplusplus
extern "C" {
#endif

struct device_metadata {
    int device_id;
    uint64_t device_size;
    char *device_name;
};

struct disk_usage {
    // Used bytes on the disk.
    uint64_t used;
    // Total bytes on the disk.
    uint64_t total;
    // Available bytes on the disk.
    uint64_t available;
};

struct status {
    char *pool_name;
    char *data_file;
    char *metadata_file;
    char *base_device_fs;
    char *library_version;
    struct disk_usage metadata;
    struct disk_usage data;
    uint64_t base_device_size;
    uint64_t sector_size;
    uint64_t min_free_space;
    bool udev_sync_supported;
    bool deferred_remove_enabled;
    bool deferred_delete_enabled;
    unsigned int deferred_deleted_device_count;
    int semusz;
    int semmni;
    char *sem_msg;
};

int device_set_init(struct graphdriver *driver, const char *drvier_home, const char **options, size_t len);

int add_device(const char *hash, const char *base_hash, struct device_set *devset,
               const json_map_string_string *storage_opts);

int mount_device(const char *hash, const char *path, const struct driver_mount_opts *mount_opts,
                 struct device_set *devset);

int unmount_device(const char *hash, const char *mount_path, struct device_set *devset);

bool has_device(const char *hash, struct device_set *devset);

int delete_device(const char *hash, bool sync_delete, struct device_set *devset);

int export_device_metadata(struct device_metadata *dev_metadata, const char *hash, struct device_set *devset);

struct status *device_set_status(struct device_set *devset);

void free_devmapper_status(struct status *st);

int device_set_shutdown(struct device_set *devset, const char *home);

void free_device_set(struct device_set *devset);

#ifdef __cplusplus
}
#endif

#endif
