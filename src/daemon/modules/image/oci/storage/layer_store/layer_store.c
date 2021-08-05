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
 * Author: liuhao
 * Create: 2020-03-26
 * Description: provide layer store functions
 ******************************************************************************/
#define _GNU_SOURCE
#include "layer_store.h"

#include <pthread.h>
#include <stdio.h>
#include <limits.h>
#include <dirent.h>
#include <isula_libutils/container_inspect.h>
#include <isula_libutils/storage_layer.h>
#include <isula_libutils/storage_mount_point.h>
#include <isula_libutils/json_common.h>
#include <isula_libutils/log.h>
#include <isula_libutils/storage_entry.h>
#include <isula_libutils/go_crc64.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "storage.h"
#include "layer.h"
#include "driver.h"
#include "linked_list.h"
#include "map.h"
#include "utils_timestamp.h"
#include "utils.h"
#include "utils_array.h"
#include "utils_file.h"
#include "util_gzip.h"
#include "buffer.h"
#include "http.h"
#include "utils_base64.h"
#include "constants.h"

#define PAYLOAD_CRC_LEN 12

struct io_read_wrapper;

typedef struct __layer_store_metadata_t {
    pthread_rwlock_t rwlock;
    map_t *by_id;
    map_t *by_name;
    map_t *by_compress_digest;
    map_t *by_uncompress_digest;
    struct linked_list layers_list;
    size_t layers_list_len;
} layer_store_metadata;

typedef struct digest_layer {
    struct linked_list layer_list;
    size_t layer_list_len;
} digest_layer_t;

typedef struct {
    FILE *tmp_file;
    storage_entry *entry;
} tar_split;

static layer_store_metadata g_metadata;
static char *g_root_dir;
static char *g_run_dir;

static inline char *tar_split_path(const char *id);
static inline char *mountpoint_json_path(const char *id);
static inline char *layer_json_path(const char *id);

static int insert_digest_into_map(map_t *by_digest, const char *digest, const char *id);
static int delete_digest_from_map(map_t *by_digest, const char *digest, const char *id);

static bool remove_name(const char *name);

#define READ_BLOCK_SIZE 10240

static inline bool layer_store_lock(bool writable)
{
    int nret = 0;

    if (writable) {
        nret = pthread_rwlock_wrlock(&g_metadata.rwlock);
    } else {
        nret = pthread_rwlock_rdlock(&g_metadata.rwlock);
    }
    if (nret != 0) {
        ERROR("Lock memory store failed: %s", strerror(nret));
        return false;
    }

    return true;
}

static inline void layer_store_unlock()
{
    int nret = 0;

    nret = pthread_rwlock_unlock(&g_metadata.rwlock);
    if (nret != 0) {
        FATAL("Unlock memory store failed: %s", strerror(nret));
    }
}

void layer_store_cleanup()
{
    struct linked_list *item = NULL;
    struct linked_list *next = NULL;

    map_free(g_metadata.by_id);
    g_metadata.by_id = NULL;
    map_free(g_metadata.by_name);
    g_metadata.by_name = NULL;
    map_free(g_metadata.by_compress_digest);
    g_metadata.by_compress_digest = NULL;
    map_free(g_metadata.by_uncompress_digest);
    g_metadata.by_uncompress_digest = NULL;

    linked_list_for_each_safe(item, &(g_metadata.layers_list), next) {
        linked_list_del(item);
        layer_ref_dec((layer_t *)item->elem);
        free(item);
        item = NULL;
    }
    g_metadata.layers_list_len = 0;

    pthread_rwlock_destroy(&(g_metadata.rwlock));

    free(g_run_dir);
    g_run_dir = NULL;
    free(g_root_dir);
    g_root_dir = NULL;
}

/* layers map kvfree */
static void layer_map_kvfree(void *key, void *value)
{
    free(key);
}

static void free_digest_layer_t(digest_layer_t *ptr)
{
    struct linked_list *item = NULL;
    struct linked_list *next = NULL;

    if (ptr == NULL) {
        return;
    }

    linked_list_for_each_safe(item, &(ptr->layer_list), next) {
        linked_list_del(item);
        free(item->elem);
        item->elem = NULL;
        free(item);
        item = NULL;
    }

    ptr->layer_list_len = 0;
    free(ptr);
}

static void digest_map_kvfree(void *key, void *value)
{
    digest_layer_t *val = (digest_layer_t *)value;

    free(key);
    free_digest_layer_t(val);
}

static inline void insert_g_layer_list_item(struct linked_list *item)
{
    if (item == NULL) {
        return;
    }

    linked_list_add_tail(&g_metadata.layers_list, item);
    g_metadata.layers_list_len += 1;
}

static bool append_layer_into_list(layer_t *l)
{
    struct linked_list *item = NULL;

    if (l == NULL) {
        return true;
    }

    item = util_smart_calloc_s(sizeof(struct linked_list), 1);
    if (item == NULL) {
        ERROR("Out of memory");
        return false;
    }

    linked_list_add_elem(item, l);

    insert_g_layer_list_item(item);
    return true;
}

static inline void delete_g_layer_list_item(struct linked_list *item)
{
    if (item == NULL) {
        return;
    }

    linked_list_del(item);

    layer_ref_dec((layer_t *)item->elem);
    item->elem = NULL;
    free(item);
    g_metadata.layers_list_len -= 1;
}

void remove_layer_list_tail()
{
    struct linked_list *item = NULL;

    if (linked_list_empty(&g_metadata.layers_list)) {
        return;
    }

    item = g_metadata.layers_list.prev;

    delete_g_layer_list_item(item);
}

static bool init_from_conf(const struct storage_module_init_options *conf)
{
    int nret = 0;
    char *tmp_path = NULL;

    if (conf == NULL) {
        return false;
    }

    if (conf->storage_root == NULL || conf->storage_run_root == NULL || conf->driver_name == NULL) {
        ERROR("Invalid argument");
        return false;
    }
    nret = asprintf(&tmp_path, "%s/%s-layers", conf->storage_run_root, conf->driver_name);
    if (nret < 0 || nret > PATH_MAX) {
        SYSERROR("Create run root path failed");
        goto free_out;
    }
    g_run_dir = tmp_path;
    tmp_path = NULL;
    nret = asprintf(&tmp_path, "%s/%s-layers", conf->storage_root, conf->driver_name);
    if (nret < 0 || nret > PATH_MAX) {
        SYSERROR("Create root path failed");
        goto free_out;
    }

    nret = graphdriver_init(conf);
    if (nret != 0) {
        goto free_out;
    }
    g_root_dir = tmp_path;
    tmp_path = NULL;

    return true;
free_out:
    free(g_run_dir);
    g_run_dir = NULL;
    free(g_root_dir);
    g_root_dir = NULL;
    free(tmp_path);
    return false;
}

static int do_validate_image_layer(const char *path, layer_t *l)
{
    char *tspath = NULL;
    int ret = 0;

    // it is not a layer of image
    if (l->slayer->diff_digest == NULL) {
        return 0;
    }

    tspath = tar_split_path(l->slayer->id);
    if (!util_file_exists(tspath) || !graphdriver_layer_exists(l->slayer->id)) {
        ERROR("Invalid data of layer: %s remove it", l->slayer->id);
        ret = -1;
    }

    free(tspath);
    return ret;
}
// 挂载
static int update_mount_point(layer_t *l)
{
    container_inspect_graph_driver *d_meta = NULL;
    int ret = 0;

    if (l->smount_point == NULL) {
        l->smount_point = util_common_calloc_s(sizeof(storage_mount_point));
        if (l->smount_point == NULL) {
            ERROR("Out of memory");
            return -1;
        }
    }

    d_meta = graphdriver_get_metadata(l->slayer->id);
    if (d_meta == NULL) {
        ERROR("Get metadata of driver failed");
        ret = -1;
        goto out;
    }
    if (d_meta->data != NULL) {
        free(l->smount_point->path);
        l->smount_point->path = util_strdup_s(d_meta->data->merged_dir);
    }

    if (l->mount_point_json_path == NULL) {
        l->mount_point_json_path = mountpoint_json_path(l->slayer->id);
        if (l->mount_point_json_path == NULL) {
            ERROR("Failed to get layer %s mount point json", l->slayer->id);
            ret = -1;
            goto out;
        }
    }

out:
    free_container_inspect_graph_driver(d_meta);
    return ret;
}

static struct driver_mount_opts *fill_driver_mount_opts(const layer_t *l)
{
    struct driver_mount_opts *d_opts = NULL;

    d_opts = util_common_calloc_s(sizeof(struct driver_mount_opts));
    if (d_opts == NULL) {
        ERROR("Out of meoroy");
        goto err_out;
    }

    if (l->slayer->mountlabel != NULL) {
        d_opts->mount_label = util_strdup_s(l->slayer->mountlabel);
    }

    return d_opts;

err_out:
    free_graphdriver_mount_opts(d_opts);
    return NULL;
}

static char *mount_helper(layer_t *l)
{
    char *mount_point = NULL;
    int nret = 0;
    struct driver_mount_opts *d_opts = NULL;
    // 更新挂载点目录信息
    nret = update_mount_point(l);
    if (nret != 0) {
        ERROR("Failed to update mount point");
        return NULL;
    }

    if (l->smount_point->count > 0) {
        l->smount_point->count += 1;
        mount_point = util_strdup_s(l->smount_point->path);
        goto save_json;
    }

    d_opts = fill_driver_mount_opts(l);
    if (d_opts == NULL) {
        ERROR("Failed to fill layer %s driver mount opts", l->slayer->id);
        goto out;
    }

    mount_point = graphdriver_mount_layer(l->slayer->id, d_opts);
    if (mount_point == NULL) {
        ERROR("Call driver mount: %s failed", l->slayer->id);
        goto out;
    }

    l->smount_point->count += 1;

save_json:
    (void)save_mount_point(l);

out:
    free_graphdriver_mount_opts(d_opts);
    return mount_point;
}

static inline char *tar_split_tmp_path(const char *id)
{
    char *result = NULL;
    int nret = 0;

    nret = asprintf(&result, "%s/%s/%s.tar-split", g_root_dir, id, id);
    if (nret < 0 || nret > PATH_MAX) {
        SYSERROR("Create tar split path failed");
        return NULL;
    }

    return result;
}

static inline char *tar_split_path(const char *id)
{
    char *result = NULL;
    int nret = 0;

    nret = asprintf(&result, "%s/%s/%s.tar-split.gz", g_root_dir, id, id);
    if (nret < 0 || nret > PATH_MAX) {
        SYSERROR("Create tar split path failed");
        return NULL;
    }

    return result;
}

static inline char *layer_json_path(const char *id)
{
    char *result = NULL;
    int nret = 0;

    nret = asprintf(&result, "%s/%s/layer.json", g_root_dir, id);
    if (nret < 0 || nret > PATH_MAX) {
        SYSERROR("Create layer json path failed");
        return NULL;
    }

    return result;
}

static inline char *mountpoint_json_path(const char *id)
{
    char *result = NULL;
    int nret = 0;

    nret = asprintf(&result, "%s/%s.json", g_run_dir, id);
    if (nret < 0 || nret > PATH_MAX) {
        SYSERROR("Create mount point json path failed");
        return NULL;
    }

    return result;
}

static layer_t *lookup(const char *id)
{
    layer_t *l = NULL;

    l = map_search(g_metadata.by_id, (void *)id);
    if (l != NULL) {
        goto out;
    }
    l = map_search(g_metadata.by_name, (void *)id);
    if (l != NULL) {
        goto out;
    }
    DEBUG("can not found layer: %s", id);

    return NULL;
out:
    layer_ref_inc(l);
    return l;
}

static inline layer_t *lookup_with_lock(const char *id)
{
    layer_t *ret = NULL;

    if (!layer_store_lock(false)) {
        return NULL;
    }

    ret = lookup(id);
    layer_store_unlock();
    return ret;
}

static int driver_create_layer(const char *id, const char *parent, bool writable,
                               const struct layer_store_mount_opts *opt)
{
    struct driver_create_opts c_opts = { 0 };
    int ret = 0;
    size_t i = 0;

    if (opt != NULL) {
        c_opts.mount_label = util_strdup_s(opt->mount_label);
        if (opt->mount_opts != NULL) {
            c_opts.storage_opt = util_smart_calloc_s(sizeof(json_map_string_string), 1);
            if (c_opts.storage_opt == NULL) {
                ERROR("Out of memory");
                ret = -1;
                goto free_out;
            }
            for (i = 0; i < opt->mount_opts->len; i++) {
                ret = append_json_map_string_string(c_opts.storage_opt, opt->mount_opts->keys[i],
                                                    opt->mount_opts->values[i]);
                if (ret != 0) {
                    ERROR("Out of memory");
                    goto free_out;
                }
            }
        }
    }

    if (writable) {
        ret = graphdriver_create_rw(id, parent, &c_opts);
    } else {
        ret = graphdriver_create_ro(id, parent, &c_opts);
    }
    if (ret != 0) {
        if (id != NULL) {
            ERROR("error creating %s layer with ID %s", writable ? "read-write" : "", id);
        } else {
            ERROR("error creating %s layer", writable ? "read-write" : "");
        }
        goto free_out;
    }

free_out:
    free(c_opts.mount_label);
    free_json_map_string_string(c_opts.storage_opt);
    return ret;
}

static int update_layer_datas(const char *id, const struct layer_opts *opts, layer_t *l)
{
    int ret = 0;
    storage_layer *slayer = NULL;
    char timebuffer[TIME_STR_SIZE] = { 0 };
    size_t i = 0;

    slayer = util_smart_calloc_s(sizeof(storage_layer), 1);
    if (slayer == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto free_out;
    }

    slayer->id = util_strdup_s(id);
    slayer->parent = util_strdup_s(opts->parent);
    if (opts->opts != NULL) {
        slayer->mountlabel = util_strdup_s(opts->opts->mount_label);
    }
    if (!util_get_now_local_utc_time_buffer(timebuffer, TIME_STR_SIZE)) {
        ERROR("Get create time failed");
        ret = -1;
        goto free_out;
    }
    slayer->created = util_strdup_s(timebuffer);

    if (opts->names_len > 0) {
        slayer->names = util_smart_calloc_s(sizeof(char *), opts->names_len);
        if (slayer->names == NULL) {
            ERROR("Out of memory");
            ret = -1;
            goto free_out;
        }
    }
    for (i = 0; i < opts->names_len; i++) {
        slayer->names[i] = util_strdup_s(opts->names[i]);
        slayer->names_len++;
    }
    slayer->diff_digest = util_strdup_s(opts->uncompressed_digest);
    slayer->compressed_diff_digest = util_strdup_s(opts->compressed_digest);

    l->layer_json_path = layer_json_path(id);
    if (l->layer_json_path == NULL) {
        ret = -1;
        goto free_out;
    }

    l->slayer = slayer;

free_out:
    if (ret != 0) {
        free_storage_layer(slayer);
    }
    return ret;
}

static int delete_digest_from_map(map_t *by_digest, const char *digest, const char *id)
{
    digest_layer_t *old_list = NULL;
    struct linked_list *item = NULL;
    struct linked_list *next = NULL;

    if (digest == NULL) {
        return 0;
    }

    old_list = (digest_layer_t *)map_search(by_digest, (void *)digest);
    if (old_list == NULL) {
        return 0;
    }

    linked_list_for_each_safe(item, &(old_list->layer_list), next) {
        char *t_id = (char *)item->elem;
        if (strcmp(t_id, id) == 0) {
            linked_list_del(item);
            free(item->elem);
            item->elem = NULL;
            free(item);
            old_list->layer_list_len -= 1;
            break;
        }
    }

    if (old_list->layer_list_len == 0 && !map_remove(by_digest, (void *)digest)) {
        WARN("Remove old failed");
    }

    return 0;
}

static int insert_new_digest_list(map_t *by_digest, const char *digest, struct linked_list *item)
{
    digest_layer_t *new_list = NULL;

    new_list = (digest_layer_t *)util_common_calloc_s(sizeof(digest_layer_t));
    if (new_list == NULL) {
        ERROR("Out of memory");
        return -1;
    }

    linked_list_init(&(new_list->layer_list));
    linked_list_add_tail(&(new_list->layer_list), item);
    new_list->layer_list_len += 1;
    if (!map_insert(by_digest, (void *)digest, (void *)new_list)) {
        linked_list_del(item);
        goto free_out;
    }

    return 0;
free_out:
    free_digest_layer_t(new_list);
    return -1;
}

static int insert_digest_into_map(map_t *by_digest, const char *digest, const char *id)
{
    digest_layer_t *old_list = NULL;
    struct linked_list *item = NULL;

    if (digest == NULL) {
        INFO("Layer: %s with empty digest", id);
        return 0;
    }

    item = (struct linked_list *)util_common_calloc_s(sizeof(struct linked_list));
    if (item == NULL) {
        ERROR("Out of memory");
        return -1;
    }
    linked_list_add_elem(item, (void *)util_strdup_s(id));

    old_list = (digest_layer_t *)map_search(by_digest, (void *)digest);
    if (old_list == NULL) {
        if (insert_new_digest_list(by_digest, digest, item) != 0) {
            ERROR("Insert new digest: %s failed", digest);
            goto free_out;
        }
    } else {
        linked_list_add_tail(&(old_list->layer_list), item);
        old_list->layer_list_len += 1;
    }

    return 0;
free_out:
    free(item->elem);
    free(item);
    return -1;
}

static int remove_memory_stores(const char *id)
{
    struct linked_list *item = NULL;
    struct linked_list *next = NULL;
    layer_t *l = NULL;
    size_t i = 0;
    int ret = 0;

    l = lookup(id);
    if (l == NULL) {
        ERROR("layer not known");
        return -1;
    }

    if (delete_digest_from_map(g_metadata.by_compress_digest, l->slayer->compressed_diff_digest, l->slayer->id) != 0) {
        ERROR("Remove %s from compress digest failed", id);
        ret = -1;
        goto out;
    }
    if (delete_digest_from_map(g_metadata.by_uncompress_digest, l->slayer->diff_digest, l->slayer->id) != 0) {
        // ignore this error, because only happen at out of memory;
        // we cannot to recover before operator, so just go on.
        WARN("Remove %s from uncompress failed", id);
    }

    if (!map_remove(g_metadata.by_id, (void *)l->slayer->id)) {
        WARN("Remove by id: %s failed", id);
    }

    for (; i < l->slayer->names_len; i++) {
        if (!map_remove(g_metadata.by_name, (void *)l->slayer->names[i])) {
            WARN("Remove by name: %s failed", l->slayer->names[i]);
        }
    }

    linked_list_for_each_safe(item, &(g_metadata.layers_list), next) {
        layer_t *tl = (layer_t *)item->elem;
        if (strcmp(tl->slayer->id, id) != 0) {
            continue;
        }
        delete_g_layer_list_item(item);
        break;
    }

out:
    layer_ref_dec(l);
    return ret;
}

static int insert_memory_stores(const char *id, const struct layer_opts *opts, layer_t *l)
{
    int ret = 0;
    int i = 0;

    if (!append_layer_into_list(l)) {
        ret = -1;
        goto out;
    }

    if (!map_insert(g_metadata.by_id, (void *)id, (void *)l)) {
        ERROR("Update by id failed");
        ret = -1;
        goto clear_list;
    }

    for (; i < opts->names_len; i++) {
        if (!map_insert(g_metadata.by_name, (void *)opts->names[i], (void *)l)) {
            ERROR("Update by names failed");
            ret = -1;
            goto clear_by_name;
        }
    }

    if (l->slayer->compressed_diff_digest != NULL) {
        ret = insert_digest_into_map(g_metadata.by_compress_digest, l->slayer->compressed_diff_digest, id);
        if (ret != 0) {
            goto clear_by_name;
        }
    }

    if (l->slayer->diff_digest != NULL) {
        ret = insert_digest_into_map(g_metadata.by_uncompress_digest, l->slayer->diff_digest, id);
        if (ret != 0) {
            goto clear_compress_digest;
        }
    }

    goto out;
clear_compress_digest:
    if (l->slayer->compressed_diff_digest != NULL) {
        (void)delete_digest_from_map(g_metadata.by_compress_digest, l->slayer->compressed_diff_digest, id);
    }
clear_by_name:
    for (i = i - 1; i >= 0; i--) {
        if (!map_remove(g_metadata.by_name, (void *)opts->names[i])) {
            WARN("Remove name: %s failed", opts->names[i]);
        }
    }
    if (!map_remove(g_metadata.by_id, (void *)id)) {
        WARN("Remove layer: %s failed", id);
    }
clear_list:
    remove_layer_list_tail();
out:
    return ret;
}

static void free_archive_read(struct archive *read_a)
{
    if (read_a == NULL) {
        return;
    }
    archive_read_close(read_a);
    archive_read_free(read_a);
}

static struct archive *create_archive_read(int fd)
{
    int nret = 0;
    struct archive *ret = NULL;

    ret = archive_read_new();
    if (ret == NULL) {
        ERROR("Out of memory");
        return NULL;
    }
    nret = archive_read_support_filter_all(ret);
    if (nret != 0) {
        ERROR("archive read support compression all failed");
        goto err_out;
    }
    nret = archive_read_support_format_all(ret);
    if (nret != 0) {
        ERROR("archive read support format all failed");
        goto err_out;
    }
    nret = archive_read_open_fd(ret, fd, READ_BLOCK_SIZE);
    if (nret != 0) {
        ERROR("archive read open file failed: %s", archive_error_string(ret));
        goto err_out;
    }

    return ret;
err_out:
    free_archive_read(ret);
    return NULL;
}

typedef int (*archive_entry_cb_t)(struct archive_entry *entry, struct archive *ar, int32_t position, Buffer *json_buf,
                                  int64_t *size);

static void free_storage_entry_data(storage_entry *entry)
{
    if (entry->name != NULL) {
        free(entry->name);
        entry->name = NULL;
    }
    if (entry->payload != NULL) {
        free(entry->payload);
        entry->payload = NULL;
    }
}

static char *caculate_playload(struct archive *ar)
{
    int r = 0;
    unsigned char *block_buf = NULL;
    size_t block_size = 0;
#if ARCHIVE_VERSION_NUMBER >= 3000000
    int64_t block_offset = 0;
#else
    off_t block_offset = 0;
#endif
    char *ret = NULL;
    int nret = 0;
    const isula_crc_table_t *ctab = NULL;
    uint64_t crc = 0;
    // max crc bits is 8
    unsigned char sum_data[8] = { 0 };
    // add \0 at crc bits last, so need a 9 bits array
    unsigned char tmp_data[9] = { 0 };
    bool empty = true;

    ctab = new_isula_crc_table(ISO_POLY);

    if (ctab == NULL) {
        return NULL;
    }

    for (;;) {
        r = archive_read_data_block(ar, (const void **)&block_buf, &block_size, &block_offset);
        if (r == ARCHIVE_EOF) {
            break;
        }
        if (r != ARCHIVE_OK) {
            nret = -1;
            break;
        }
        if (!isula_crc_update(ctab, &crc, block_buf, block_size)) {
            nret = -1;
            break;
        }
        empty = false;
    }
    if (empty) {
        goto out;
    }

    isula_crc_sum(crc, sum_data);
    // max crc bits is 8
    for (r = 0; r < 8; r++) {
        tmp_data[r] = sum_data[r];
    }
    nret = util_base64_encode(tmp_data, 8, &ret);

    if (nret != 0) {
        return NULL;
    }

out:
    return ret;
}

static int archive_entry_parse(struct archive_entry *entry, struct archive *ar, int32_t position, Buffer *json_buf,
                               int64_t *size)
{
    storage_entry sentry = { 0 };
    struct parser_context ctx = { OPT_GEN_SIMPLIFY, stderr };
    parser_error jerr = NULL;
    char *data = NULL;
    int ret = -1;
    ssize_t nret = 0;

    // get entry information: name, size
    sentry.type = 1;
    sentry.name = util_strdup_s(archive_entry_pathname(entry));
    sentry.size = archive_entry_size(entry);
    sentry.position = position;
    // caculate playload
    sentry.payload = caculate_playload(ar);

    data = storage_entry_generate_json(&sentry, &ctx, &jerr);
    if (data == NULL) {
        ERROR("parse entry failed: %s", jerr);
        goto out;
    }
    nret = buffer_append(json_buf, data, strlen(data));
    if (nret != 0) {
        goto out;
    }
    nret = buffer_append(json_buf, "\n", 1);
    if (nret != 0) {
        goto out;
    }

    *size = *size + sentry.size;

    ret = 0;
out:
    free_storage_entry_data(&sentry);
    free(data);
    free(jerr);
    return ret;
}

static int foreach_archive_entry(archive_entry_cb_t cb, int fd, const char *dist, int64_t *size)
{
    int ret = -1;
    int nret = 0;
    struct archive *read_a = NULL;
    struct archive_entry *entry = NULL;
    int32_t position = 0;
    Buffer *json_buf = NULL;

    // we need reset fd point to first position
    if (lseek(fd, 0, SEEK_SET) == -1) {
        ERROR("can not reposition of archive file");
        goto out;
    }
    json_buf = buffer_alloc(HTTP_GET_BUFFER_SIZE);
    if (json_buf == NULL) {
        ERROR("Failed to malloc output_buffer");
        goto out;
    }

    read_a = create_archive_read(fd);
    if (read_a == NULL) {
        goto out;
    }

    for (;;) {
        nret = archive_read_next_header(read_a, &entry);
        if (nret == ARCHIVE_EOF) {
            DEBUG("read entry: %d", position);
            break;
        }
        if (nret != ARCHIVE_OK) {
            ERROR("archive read header failed: %s", archive_error_string(read_a));
            goto out;
        }
        nret = cb(entry, read_a, position, json_buf, size);
        if (nret != 0) {
            goto out;
        }
        position++;
    }
    nret = util_atomic_write_file(dist, json_buf->contents, json_buf->bytes_used, SECURE_CONFIG_FILE_MODE, true);
    if (nret != 0) {
        ERROR("save tar split failed");
        goto out;
    }

    ret = 0;
out:
    buffer_free(json_buf);
    free_archive_read(read_a);
    return ret;
}

static int make_tar_split_file(const char *lid, const struct io_read_wrapper *diff, int64_t *size)
{
    int *pfd = (int *)diff->context;
    char *save_fname = NULL;
    char *save_fname_gz = NULL;
    int ret = -1;
    int tfd = -1;

    save_fname = tar_split_tmp_path(lid);
    if (save_fname == NULL) {
        return -1;
    }
    save_fname_gz = tar_split_path(lid);
    if (save_fname_gz == NULL) {
        goto out;
    }
    // step 1: read header;
    tfd = util_open(save_fname, O_WRONLY | O_CREAT, SECURE_CONFIG_FILE_MODE);
    if (tfd == -1) {
        SYSERROR("touch file failed");
        goto out;
    }
    close(tfd);
    tfd = -1;

    // step 2: build entry json;
    // step 3: write into tar split;
    ret = foreach_archive_entry(archive_entry_parse, *pfd, save_fname, size);
    if (ret != 0) {
        goto out;
    }

    // not exist entry for layer, just return 0
    if (!util_file_exists(save_fname)) {
        goto out;
    }

    // step 4: gzip tar split, and save file.
    ret = util_gzip_z(save_fname, save_fname_gz, SECURE_CONFIG_FILE_MODE);

    // always remove tmp tar split file, even though gzip failed.
    // if remove failed, just log message
    if (util_path_remove(save_fname) != 0) {
        WARN("remove tmp tar split failed");
    }

out:
    free(save_fname_gz);
    free(save_fname);
    return ret;
}

static int apply_diff(layer_t *l, const struct io_read_wrapper *diff)
{
    int64_t size = 0;
    int ret = 0;

    if (diff == NULL) {
        return 0;
    }

    ret = graphdriver_apply_diff(l->slayer->id, diff);
    if (ret != 0) {
        goto out;
    }

    // uncompress digest get from up caller
    ret = make_tar_split_file(l->slayer->id, diff, &size);

    INFO("Apply layer get size: %ld", size);
    l->slayer->diff_size = size;

out:
    return ret;
}

static bool build_layer_dir(const char *id)
{
    char *result = NULL;
    int nret = 0;
    bool ret = true;

    nret = asprintf(&result, "%s/%s", g_root_dir, id);
    if (nret < 0 || nret > PATH_MAX) {
        SYSERROR("Create layer json path failed");
        return false;
    }

    if (util_mkdir_p(result, IMAGE_STORE_PATH_MODE) != 0) {
        ret = false;
    }

    free(result);
    return ret;
}

static int new_layer_by_opts(const char *id, const struct layer_opts *opts)
{
    int ret = 0;
    layer_t *l = NULL;

    l = create_empty_layer();
    if (l == NULL) {
        ret = -1;
        goto out;
    }
    if (!build_layer_dir(id)) {
        ret = -1;
        goto out;
    }

    ret = update_layer_datas(id, opts, l);
    if (ret != 0) {
        goto out;
    }

    // update memory store
    ret = insert_memory_stores(id, opts, l);

out:
    if (ret != 0) {
        layer_ref_dec(l);
    }
    return ret;
}

static int layer_store_remove_layer(const char *id)
{
    char *rpath = NULL;
    int ret = 0;
    int nret = 0;

    if (id == NULL) {
        return 0;
    }

    nret = asprintf(&rpath, "%s/%s", g_root_dir, id);
    if (nret < 0 || nret > PATH_MAX) {
        SYSERROR("Create layer json path failed");
        return -1;
    }

    ret = util_recursive_rmdir(rpath, 0);
    free(rpath);
    return ret;
}

int layer_set_hold_refs(const char *layer_id, bool increase)
{
    layer_t *l = NULL;
    int ret = 0;

    if (layer_id == NULL) {
        ERROR("Invalid NULL layer id when set hold refs");
        return -1;
    }

    if (!layer_store_lock(true)) {
        ERROR("Failed to lock layer store, reset hold refs for layer %s failed", layer_id);
        return -1;
    }

    l = map_search(g_metadata.by_id, (void *)layer_id);
    if (l == NULL) {
        ERROR("layer %s not found when set hold refs", layer_id);
        ret = -1;
        goto out;
    }
    if (increase) {
        l->hold_refs_num++;
    } else {
        l->hold_refs_num--;
    }

out:
    layer_store_unlock();

    return ret;
}

int layer_inc_hold_refs(const char *layer_id)
{
    return layer_set_hold_refs(layer_id, true);
}

int layer_dec_hold_refs(const char *layer_id)
{
    return layer_set_hold_refs(layer_id, false);
}

int layer_get_hold_refs(const char *layer_id, int *refs_num)
{
    int ret = 0;
    layer_t *l = NULL;

    if (layer_id == NULL || refs_num == NULL) {
        ERROR("Invalid NULL param when get hold refs");
        return -1;
    }

    if (!layer_store_lock(true)) {
        ERROR("Failed to lock layer store, get hold refs of layer %s failed", layer_id);
        return -1;
    }

    l = map_search(g_metadata.by_id, (void *)layer_id);
    if (l == NULL) {
        ERROR("layer %s not found when get hold refs", layer_id);
        ret = -1;
        goto out;
    }
    *refs_num = l->hold_refs_num;

out:
    layer_store_unlock();

    return ret;
}

int layer_store_create(const char *id, const struct layer_opts *opts, const struct io_read_wrapper *diff, char **new_id)
{
    int ret = 0;
    char *lid = NULL;
    layer_t *l = NULL;

    if (opts == NULL) {
        ERROR("Invalid argument");
        return -1;
    }

    if (!layer_store_lock(true)) {
        return -1;
    }

    lid = util_strdup_s(id);

    // If the layer already exist, increase refs number to hold the layer is enough
    l = lookup(lid);
    if (l != NULL) {
        l->hold_refs_num++; // increase refs number, so others can't delete this layer
        goto free_out;
    }

    // create layer by driver
    ret = driver_create_layer(lid, opts->parent, opts->writable, opts->opts);
    if (ret != 0) {
        goto free_out;
    }
    ret = new_layer_by_opts(lid, opts);
    if (ret != 0) {
        goto driver_remove;
    }

    l = lookup(lid);
    if (l == NULL) {
        ret = -1;
        goto clear_memory;
    }
    l->slayer->incompelte = true;
    if (save_layer(l) != 0) {
        ret = -1;
        goto clear_memory;
    }

    ret = apply_diff(l, diff);
    if (ret != 0) {
        goto clear_memory;
    }
    ret = update_mount_point(l);
    if (ret != 0) {
        goto clear_memory;
    }

    l->slayer->incompelte = false;

    ret = save_layer(l);
    if (ret == 0) {
        DEBUG("create layer success");
        if (new_id != NULL) {
            *new_id = lid;
            lid = NULL;
        }
        l->hold_refs_num++; // increase refs number, so others can't delete this layer
        goto free_out;
    }
    ERROR("Save layer failed");
clear_memory:
    (void)remove_memory_stores(lid);
driver_remove:
    if (ret != 0) {
        (void)graphdriver_rm_layer(lid);
        (void)layer_store_remove_layer(lid);
    }
free_out:
    layer_store_unlock();
    layer_ref_dec(l);
    free(lid);
    return ret;
}

static int umount_helper(layer_t *l, bool force)
{
    int ret = 0;

    if (l->smount_point == NULL) {
        return 0;
    }

    if (!force && l->smount_point->count > 1) {
        l->smount_point->count -= 1;
        goto save_json;
    }

    // not exist file error need to ignore
    ret = graphdriver_umount_layer(l->slayer->id);
    if (ret != 0) {
        ERROR("Call driver umount failed");
        ret = -1;
        goto out;
    }
    l->smount_point->count = 0;

save_json:
    (void)save_mount_point(l);
out:
    return ret;
}

static int do_delete_layer(const char *id)
{
    int ret = 0;
    layer_t *l = NULL;
    char *tspath = NULL;

    l = lookup(id);
    if (l == NULL) {
        WARN("layer %s not exists already, return success", id);
        goto free_out;
    }

    if (umount_helper(l, true) != 0) {
        ret = -1;
        ERROR("Failed to umount layer %s", l->slayer->id);
        goto free_out;
    }

    if (l->mount_point_json_path != NULL && util_path_remove(l->mount_point_json_path) != 0) {
        SYSERROR("Can not remove mount point file of layer %s, just ignore.", l->mount_point_json_path);
    }

    tspath = tar_split_path(l->slayer->id);
    if (tspath != NULL && util_path_remove(tspath) != 0) {
        SYSERROR("Can not remove layer files, just ignore.");
    }

    ret = remove_memory_stores(l->slayer->id);
    if (ret != 0) {
        goto free_out;
    }

    ret = graphdriver_rm_layer(l->slayer->id);
    if (ret != 0) {
        ERROR("Remove layer: %s by driver failed", l->slayer->id);
        goto free_out;
    }

    ret = layer_store_remove_layer(l->slayer->id);

free_out:
    free(tspath);
    layer_ref_dec(l);
    return ret;
}

int layer_store_delete(const char *id)
{
    int ret = 0;

    if (id == NULL) {
        return -1;
    }

    if (!layer_store_lock(true)) {
        return -1;
    }

    if (do_delete_layer(id) != 0) {
        ERROR("Failed to delete layer %s", id);
        ret = -1;
    }

    layer_store_unlock();
    return ret;
}

bool layer_store_exists(const char *id)
{
    layer_t *l = lookup_with_lock(id);

    if (l == NULL) {
        return false;
    }

    layer_ref_dec(l);
    return true;
}

static void copy_json_to_layer(const layer_t *jl, struct layer *l)
{
    if (jl->slayer == NULL) {
        return;
    }
    l->id = util_strdup_s(jl->slayer->id);
    l->parent = util_strdup_s(jl->slayer->parent);
    l->compressed_digest = util_strdup_s(jl->slayer->compressed_diff_digest);
    l->compress_size = jl->slayer->compressed_size;
    l->uncompressed_digest = util_strdup_s(jl->slayer->diff_digest);
    l->uncompress_size = jl->slayer->diff_size;
    if (jl->smount_point != NULL) {
        l->mount_point = util_strdup_s(jl->smount_point->path);
        l->mount_count = jl->smount_point->count;
    }
}

int layer_store_list(struct layer_list *resp)
{
    struct linked_list *item = NULL;
    struct linked_list *next = NULL;
    size_t i = 0;
    int ret = 0;

    if (resp == NULL) {
        ERROR("Invalid argument");
        return -1;
    }

    if (!layer_store_lock(false)) {
        return -1;
    }

    resp->layers = (struct layer **)util_smart_calloc_s(sizeof(struct layer *), g_metadata.layers_list_len);
    if (resp->layers == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto unlock;
    }

    linked_list_for_each_safe(item, &(g_metadata.layers_list), next) {
        layer_t *l = (layer_t *)item->elem;
        resp->layers[i] = util_common_calloc_s(sizeof(struct layer));
        if (resp->layers[i] == NULL) {
            ERROR("Out of memory");
            ret = -1;
            goto unlock;
        }
        copy_json_to_layer(l, resp->layers[i]);
        i++;
        resp->layers_len += 1;
    }

unlock:
    layer_store_unlock();
    return ret;
}

static int layers_by_digest_map(map_t *m, const char *digest, struct layer_list *resp)
{
    struct linked_list *item = NULL;
    struct linked_list *next = NULL;
    int ret = -1;
    digest_layer_t *id_list = NULL;
    size_t i = 0;

    id_list = (digest_layer_t *)map_search(m, (void *)digest);
    if (id_list == NULL) {
        WARN("Not found digest: %s", digest);
        goto free_out;
    }

    if (id_list->layer_list_len == 0) {
        ret = 0;
        goto free_out;
    }

    resp->layers = (struct layer **)util_smart_calloc_s(sizeof(struct layer *), id_list->layer_list_len);
    if (resp->layers == NULL) {
        ERROR("Out of memory");
        goto free_out;
    }

    linked_list_for_each_safe(item, &(id_list->layer_list), next) {
        layer_t *l = NULL;
        resp->layers[i] = util_common_calloc_s(sizeof(struct layer));
        if (resp->layers[i] == NULL) {
            ERROR("Out of memory");
            goto free_out;
        }
        l = lookup((char *)item->elem);
        if (l == NULL) {
            ERROR("layer not known");
            goto free_out;
        }
        copy_json_to_layer(l, resp->layers[i]);
        layer_ref_dec(l);
        resp->layers_len += 1;
        i++;
    }

    ret = 0;
free_out:
    return ret;
}

int layer_store_by_compress_digest(const char *digest, struct layer_list *resp)
{
    int ret = 0;

    if (resp == NULL) {
        return -1;
    }

    if (!layer_store_lock(false)) {
        return -1;
    }

    ret = layers_by_digest_map(g_metadata.by_compress_digest, digest, resp);
    layer_store_unlock();
    return ret;
}

int layer_store_by_uncompress_digest(const char *digest, struct layer_list *resp)
{
    int ret = 0;

    if (resp == NULL) {
        return -1;
    }
    if (!layer_store_lock(false)) {
        return -1;
    }

    ret = layers_by_digest_map(g_metadata.by_uncompress_digest, digest, resp);
    layer_store_unlock();
    return ret;
}

struct layer *layer_store_lookup(const char *name)
{
    struct layer *ret = NULL;
    layer_t *l = NULL;

    if (name == NULL) {
        return ret;
    }

    l = lookup_with_lock(name);
    if (l == NULL) {
        return ret;
    }

    ret = util_common_calloc_s(sizeof(struct layer));
    if (ret == NULL) {
        ERROR("Out of memory");
        layer_ref_dec(l);
        return ret;
    }

    copy_json_to_layer(l, ret);
    layer_ref_dec(l);
    return ret;
}

char *layer_store_mount(const char *id)
{
    layer_t *l = NULL;
    char *result = NULL;

    if (id == NULL) {
        ERROR("Invalid arguments");
        return NULL;
    }

    l = lookup_with_lock(id);
    if (l == NULL) {
        ERROR("layer not known");
        return NULL;
    }
    layer_lock(l);
    // 在merged中储存数据
    result = mount_helper(l);
    if (result == NULL) {
        ERROR("Failed to mount layer %s", id);
    }
    layer_unlock(l);

    layer_ref_dec(l);

    return result;
}

int layer_store_umount(const char *id, bool force)
{
    layer_t *l = NULL;
    int ret = 0;

    if (id == NULL) {
        // ignore null id
        return 0;
    }
    l = lookup_with_lock(id);
    if (l == NULL) {
        ERROR("layer not known,skip umount");
        return 0;
    }
    layer_lock(l);
    ret = umount_helper(l, force);
    layer_unlock(l);

    layer_ref_dec(l);
    return ret;
}

static bool remove_name(const char *name)
{
    size_t i = 0;
    bool ret = false;
    layer_t *l = map_search(g_metadata.by_name, (void *)name);
    if (l == NULL) {
        return false;
    }

    layer_lock(l);
    while (i < l->slayer->names_len) {
        if (strcmp(name, l->slayer->names[i]) == 0) {
            free(l->slayer->names[i]);
            size_t j = i + 1;
            for (; j < l->slayer->names_len; j++) {
                l->slayer->names[j - 1] = l->slayer->names[j];
                l->slayer->names[j] = NULL;
            }
            l->slayer->names_len -= 1;
            ret = true;
            continue;
        }
        i++;
    }
    layer_unlock(l);

    return ret;
}

int layer_store_try_repair_lowers(const char *id)
{
    layer_t *l = NULL;
    int ret = 0;

    l = lookup_with_lock(id);
    if (l == NULL) {
        return -1;
    }
    ret = graphdriver_try_repair_lowers(id, l->slayer->parent);
    layer_ref_dec(l);

    return ret;
}

void free_layer_opts(struct layer_opts *ptr)
{
    if (ptr == NULL) {
        return;
    }
    free(ptr->parent);
    ptr->parent = NULL;
    util_free_array_by_len(ptr->names, ptr->names_len);
    ptr->names = NULL;
    ptr->names_len = 0;
    free(ptr->uncompressed_digest);
    ptr->uncompressed_digest = NULL;
    free(ptr->compressed_digest);
    ptr->compressed_digest = NULL;

    free_layer_store_mount_opts(ptr->opts);
    ptr->opts = NULL;
    free(ptr);
}

void free_layer_store_mount_opts(struct layer_store_mount_opts *ptr)
{
    if (ptr == NULL) {
        return;
    }
    free(ptr->mount_label);
    ptr->mount_label = NULL;
    free_json_map_string_string(ptr->mount_opts);
    ptr->mount_opts = NULL;
    free(ptr);
}

int layer_store_get_layer_fs_info(const char *layer_id, imagetool_fs_info *fs_info)
{
    return graphdriver_get_layer_fs_info(layer_id, fs_info);
}

static int do_validate_rootfs_layer(layer_t *l)
{
    int ret = 0;
    char *mount_point = NULL;

    // it is a layer of image, just ignore
    if (l->slayer->diff_digest != NULL) {
        return 0;
    }

    if (update_mount_point(l) != 0) {
        ERROR("Failed to update mount point");
        ret = -1;
        goto out;
    }

    // try to mount the layer, and set mount count to 1
    if (l->smount_point->count > 0) {
        l->smount_point->count = 0;
        mount_point = mount_helper(l);
        if (mount_point == NULL) {
            ERROR("Failed to mount layer %s", l->slayer->id);
            ret = -1;
            goto out;
        }
    }

out:
    free(mount_point);
    return ret;
}

static bool load_layer_json_cb(const char *path_name, const struct dirent *sub_dir, void *context)
{
#define LAYER_NAME_LEN 64
    bool flag = false;
    char tmpdir[PATH_MAX] = { 0 };
    int nret = 0;
    char *rpath = NULL;
    char *mount_point_path = NULL;
    layer_t *l = NULL;

    nret = snprintf(tmpdir, PATH_MAX, "%s/%s", path_name, sub_dir->d_name);
    if (nret < 0 || nret >= PATH_MAX) {
        ERROR("Sprintf: %s failed", sub_dir->d_name);
        goto free_out;
    }

    if (!util_dir_exists(tmpdir)) {
        // ignore non-dir
        DEBUG("%s is not directory", sub_dir->d_name);
        goto free_out;
    }

    mount_point_path = mountpoint_json_path(sub_dir->d_name);
    if (mount_point_path == NULL) {
        ERROR("Out of Memory");
        goto free_out;
    }

    if (strlen(sub_dir->d_name) != LAYER_NAME_LEN) {
        ERROR("%s is invalid subdir name", sub_dir->d_name);
        goto remove_invalid_dir;
    }

    rpath = layer_json_path(sub_dir->d_name);
    if (rpath == NULL) {
        ERROR("%s is invalid layer", sub_dir->d_name);
        goto remove_invalid_dir;
    }

    l = load_layer(rpath, mount_point_path);
    if (l == NULL) {
        ERROR("load layer: %s failed, remove it", sub_dir->d_name);
        goto remove_invalid_dir;
    }

    if (do_validate_image_layer(tmpdir, l) != 0) {
        ERROR("%s is invalid image layer", sub_dir->d_name);
        goto remove_invalid_dir;
    }

    if (do_validate_rootfs_layer(l) != 0) {
        ERROR("%s is invalid rootfs layer", sub_dir->d_name);
        goto remove_invalid_dir;
    }

    if (!append_layer_into_list(l)) {
        ERROR("Failed to append layer info to list");
        goto remove_invalid_dir;
    }

    flag = true;
    goto free_out;

remove_invalid_dir:
    (void)graphdriver_umount_layer(sub_dir->d_name);
    (void)graphdriver_rm_layer(sub_dir->d_name);
    (void)util_recursive_rmdir(tmpdir, 0);

free_out:
    free(rpath);
    free(mount_point_path);
    if (!flag) {
        free_layer_t(l);
    }
    // always return true;
    // if load layer failed, just remove it
    return true;
}

static int load_layers_from_json_files()
{
    int ret = 0;
    struct linked_list *item = NULL;
    struct linked_list *next = NULL;
    bool should_save = false;

    if (!layer_store_lock(true)) {
        return -1;
    }

    ret = util_scan_subdirs(g_root_dir, load_layer_json_cb, NULL);
    if (ret != 0) {
        goto unlock_out;
    }

    linked_list_for_each_safe(item, &(g_metadata.layers_list), next) {
        layer_t *tl = (layer_t *)item->elem;
        size_t i = 0;

        if (!map_insert(g_metadata.by_id, (void *)tl->slayer->id, (void *)tl)) {
            ERROR("Insert id: %s for layer failed", tl->slayer->id);
            ret = -1;
            goto unlock_out;
        }

        for (; i < tl->slayer->names_len; i++) {
            if (remove_name(tl->slayer->names[i])) {
                should_save = true;
            }
            if (!map_insert(g_metadata.by_name, (void *)tl->slayer->names[i], (void *)tl)) {
                ret = -1;
                ERROR("Insert name: %s for layer failed", tl->slayer->names[i]);
                goto unlock_out;
            }
        }

        ret = insert_digest_into_map(g_metadata.by_compress_digest, tl->slayer->compressed_diff_digest, tl->slayer->id);
        if (ret != 0) {
            ERROR("update layer: %s compress failed", tl->slayer->id);
            goto unlock_out;
        }

        ret = insert_digest_into_map(g_metadata.by_uncompress_digest, tl->slayer->diff_digest, tl->slayer->id);
        if (ret != 0) {
            ERROR("update layer: %s uncompress failed", tl->slayer->id);
            goto unlock_out;
        }

        // check complete
        if (tl->slayer->incompelte) {
            if (do_delete_layer(tl->slayer->id) != 0) {
                ERROR("delete layer: %s failed", tl->slayer->id);
                ret = -1;
                goto unlock_out;
            }
            should_save = true;
        }

        if (should_save && save_layer(tl) != 0) {
            ERROR("save layer: %s failed", tl->slayer->id);
            ret = -1;
            goto unlock_out;
        }
    }

    ret = 0;
    goto unlock_out;
unlock_out:
    layer_store_unlock();
    return ret;
}

int layer_store_init(const struct storage_module_init_options *conf)
{
    int nret = 0;

    if (!init_from_conf(conf)) {
        return -1;
    }

    // init manager structs
    g_metadata.layers_list_len = 0;
    linked_list_init(&g_metadata.layers_list);

    nret = pthread_rwlock_init(&(g_metadata.rwlock), NULL);
    if (nret != 0) {
        ERROR("Failed to init metadata rwlock");
        goto free_out;
    }
    g_metadata.by_id = map_new(MAP_STR_PTR, MAP_DEFAULT_CMP_FUNC, layer_map_kvfree);
    if (g_metadata.by_id == NULL) {
        ERROR("Failed to new ids map");
        goto free_out;
    }
    g_metadata.by_name = map_new(MAP_STR_PTR, MAP_DEFAULT_CMP_FUNC, layer_map_kvfree);
    if (g_metadata.by_name == NULL) {
        ERROR("Failed to new names map");
        goto free_out;
    }
    g_metadata.by_compress_digest = map_new(MAP_STR_PTR, MAP_DEFAULT_CMP_FUNC, digest_map_kvfree);
    if (g_metadata.by_compress_digest == NULL) {
        ERROR("Failed to new compress map");
        goto free_out;
    }
    g_metadata.by_uncompress_digest = map_new(MAP_STR_PTR, MAP_DEFAULT_CMP_FUNC, digest_map_kvfree);
    if (g_metadata.by_uncompress_digest == NULL) {
        ERROR("Failed to new uncompress map");
        goto free_out;
    }

    // build root dir and run dir
    // 创建container-layers目录
    nret = util_mkdir_p(g_root_dir, IMAGE_STORE_PATH_MODE);
    if (nret != 0) {
        ERROR("build root dir of layer store failed");
        goto free_out;
    }
    nret = util_mkdir_p(g_run_dir, IMAGE_STORE_PATH_MODE);
    if (nret != 0) {
        ERROR("build run dir of layer store failed");
        goto free_out;
    }

    if (load_layers_from_json_files() != 0) {
        goto free_out;
    }

    DEBUG("Init layer store success");
    return 0;
free_out:
    layer_store_cleanup();
    return -1;
}

void layer_store_exit()
{
    graphdriver_cleanup();
}

static uint64_t payload_to_crc(char *payload)
{
    int ret = 0;
    int i = 0;
    uint64_t crc = 0;
    uint8_t *crc_sums = NULL;
    size_t crc_sums_len = 0;

    ret = util_base64_decode(payload, strlen(payload), &crc_sums, &crc_sums_len);
    if (ret < 0) {
        ERROR("decode tar split payload from base64 failed, payload %s", payload);
        return -1;
    }

    for (i = 0; i < crc_sums_len; i++) {
        crc |= crc_sums[i];
        if (i == crc_sums_len - 1) {
            break;
        }
        crc <<= 8;
    }

    free(crc_sums);
    return crc;
}

static int file_crc64(char *file, uint64_t *crc, uint64_t policy)
{
#define BLKSIZE 32768
    int ret = 0;
    const isula_crc_table_t *ctab = NULL;
    int fd = 0;
    void *buffer = NULL;
    ssize_t size = 0;

    fd = util_open(file, O_RDONLY, 0);
    if (fd < 0) {
        ERROR("Open file: %s, failed: %s", file, strerror(errno));
        return -1;
    }

    ctab = new_isula_crc_table(policy);
    if (ctab == NULL || !ctab->inited) {
        ERROR("create crc table failed");
        ret = -1;
        goto out;
    }

    buffer = util_common_calloc_s(BLKSIZE);
    if (buffer == NULL) {
        ERROR("out of memory");
        ret = -1;
        goto out;
    }

    *crc = 0;
    while (true) {
        size = util_read_nointr(fd, buffer, BLKSIZE);
        if (size < 0) {
            ERROR("read file %s failed: %s", file, strerror(errno));
            ret = -1;
            break;
        } else if (size == 0) {
            break;
        }

        if (!isula_crc_update(ctab, crc, buffer, (size_t)size)) {
            ERROR("crc update failed");
            ret = -1;
            break;
        }
    }

out:

    close(fd);
    free(buffer);

    return ret;
}

static int valid_crc64(storage_entry *entry, char *rootfs)
{
    int ret = 0;
    int nret = 0;
    uint64_t crc = 0;
    uint64_t expected_crc = 0;
    char file[PATH_MAX] = { 0 };
    struct stat st;
    char *fname = NULL;

    nret = snprintf(file, PATH_MAX, "%s/%s", rootfs, entry->name);
    if (nret < 0 || nret >= PATH_MAX) {
        ERROR("snprintf %s/%s failed", rootfs, entry->name);
        ret = -1;
        goto out;
    }

    if (entry->payload == NULL) {
        if (lstat(file, &st) == 0) {
            goto out;
        }
        fname = util_path_base(file);
        // is placeholder for overlay, ignore this file
        if (fname != NULL && util_has_prefix(fname, ".wh.")) {
            goto out;
        }
        ERROR("stat file or dir: %s, failed: %s", file, strerror(errno));
        ret = -1;
    } else {
        if (strlen(entry->payload) != PAYLOAD_CRC_LEN) {
            ERROR("invalid payload %s of file %s", entry->payload, file);
            ret = -1;
            goto out;
        }

        ret = file_crc64(file, &crc, ISO_POLY);
        if (ret != 0) {
            ERROR("calc crc of file %s failed", file);
            ret = -1;
            goto out;
        }

        expected_crc = payload_to_crc(entry->payload);
        if (crc != expected_crc) {
            ERROR("file %s crc 0x%jx not as expected 0x%jx", file, crc, expected_crc);
            ret = 1;
            goto out;
        }
    }

out:
    free(fname);
    return ret;
}

static void free_tar_split(tar_split *ts)
{
    if (ts == NULL) {
        return;
    }
    free_storage_entry(ts->entry);
    ts->entry = NULL;
    if (ts->tmp_file != NULL) {
        fclose(ts->tmp_file);
        ts->tmp_file = NULL;
    }
    free(ts);
    return;
}

static tar_split *new_tar_split(layer_t *l, const char *tspath)
{
    int ret = 0;
    tar_split *ts = NULL;

    ts = util_common_calloc_s(sizeof(tar_split));
    if (ts == NULL) {
        ERROR("out of memory");
        ret = -1;
        goto out;
    }

    ts->tmp_file = tmpfile();
    if (ts->tmp_file == NULL) {
        ERROR("create tmpfile failed: %s", strerror(errno));
        ret = -1;
        goto out;
    }

    ret = util_gzip_d(tspath, ts->tmp_file);
    if (ret != 0) {
        ERROR("unzip tar split file %s failed", tspath);
        goto out;
    }

    rewind(ts->tmp_file);

out:
    if (ret != 0) {
        free_tar_split(ts);
        ts = NULL;
    }

    return ts;
}

static int next_tar_split_entry(tar_split *ts, storage_entry **entry)
{
    int ret = 0;
    int nret = 0;
    char *pline = NULL;
    size_t length = 0;
    char *errmsg = NULL;

    errno = 0;
    nret = getline(&pline, &length, ts->tmp_file);
    if (nret == -1) {
        // end of file
        if (errno == 0) {
            *entry = NULL;
        } else {
            ERROR("error read line from tar split: %s", strerror(errno));
            ret = -1;
        }
        goto out;
    }

    util_trim_newline(pline);

    if (ts->entry != NULL) {
        free_storage_entry(ts->entry);
    }
    ts->entry = storage_entry_parse_data(pline, NULL, &errmsg);
    if (ts->entry == NULL) {
        ERROR("parse tar split entry failed: %s\nline:%s", errmsg, pline);
        ret = -1;
        goto out;
    }

    *entry = ts->entry;

out:
    free(errmsg);
    free(pline);

    return ret;
}

static int do_integration_check(layer_t *l, char *rootfs)
{
#define STORAGE_ENTRY_TYPE_CRC 1
    int ret = 0;
    tar_split *ts = NULL;
    storage_entry *entry = NULL;
    char *tspath = NULL;

    tspath = tar_split_path(l->slayer->id);
    if (tspath == NULL) {
        ERROR("get tar split path of layer %s failed", l->slayer->id);
        return -1;
    }
    if (!util_file_exists(tspath)) {
        ERROR("Can not found tar split of layer: %s", l->slayer->id);
        ret = -1;
        goto out;
    }

    ts = new_tar_split(l, tspath);
    if (ts == NULL) {
        ERROR("new tar split for layer %s failed", l->slayer->id);
        ret = -1;
        goto out;
    }

    ret = next_tar_split_entry(ts, &entry);
    if (ret != 0) {
        ERROR("get next tar split entry failed");
        goto out;
    }
    while (entry != NULL) {
        if (entry->type == STORAGE_ENTRY_TYPE_CRC) {
            ret = valid_crc64(entry, rootfs);
            if (ret != 0) {
                ERROR("integration check failed, layer %s, file %s", l->slayer->id, entry->name);
                goto out;
            }
        }

        ret = next_tar_split_entry(ts, &entry);
        if (ret != 0) {
            ERROR("get next tar split entry failed");
            goto out;
        }
    }

out:
    free(tspath);
    free_tar_split(ts);

    return ret;
}

/*
 * return value:
 *   <0: operator failed
 *    0: valid layer
 *   >0: invalid layer
 * */
int layer_store_check(const char *id)
{
    int ret = 0;
    char *rootfs = NULL;

    layer_t *l = lookup_with_lock(id);
    if (l == NULL || l->slayer == NULL) {
        ERROR("layer %s not found when checking integration", id);
        return -1;
    }

    // It's a container layer, not a layer of image, ignore checking
    if (l->slayer->diff_digest == NULL) {
        goto out;
    }

    rootfs = layer_store_mount(id);
    if (rootfs == NULL) {
        ERROR("mount layer of %s failed", id);
        ret = -1;
        goto out;
    }

    ret = do_integration_check(l, rootfs);
    if (ret != 0) {
        goto out;
    }

out:
    (void)layer_store_umount(id, false);
    layer_ref_dec(l);
    free(rootfs);

    return ret;
}

container_inspect_graph_driver *layer_store_get_metadata_by_layer_id(const char *id)
{
    return graphdriver_get_metadata(id);
}
