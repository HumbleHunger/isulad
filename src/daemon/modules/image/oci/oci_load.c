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
* Author: gaohuatao
* Create: 2020-05-14
* Description: isula load operator implement
*******************************************************************************/
#include "oci_load.h"

#include <errno.h>
#include <fcntl.h>
#include <isula_libutils/image_manifest_items.h>
#include <isula_libutils/json_common.h>
#include <isula_libutils/oci_image_content_descriptor.h>
#include <isula_libutils/oci_image_manifest.h>
#include <isula_libutils/oci_image_spec.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"
#include "isula_libutils/log.h"
#include "util_archive.h"
#include "storage.h"
#include "sha256.h"
#include "mediatype.h"
#include "utils_images.h"
#include "err_msg.h"
#include "constants.h"
#include "io_wrapper.h"
#include "utils_array.h"
#include "utils_file.h"
#include "utils_verify.h"
#include "oci_image.h"

#define MANIFEST_BIG_DATA_KEY "manifest"
#define OCI_SCHEMA_VERSION 2

static image_manifest_items_element **load_manifest(const char *fname, size_t *length)
{
    image_manifest_items_element **manifest = NULL;
    parser_error err = NULL;
    size_t len = 0;

    if (fname == NULL || length == NULL) {
        return NULL;
    }

    manifest = image_manifest_items_parse_file(fname, NULL, &err, &len);
    if (manifest == NULL) {
        len = 0;
        ERROR("Parse manifest %s err:%s", fname, err);
    }

    *length = len;
    free(err);
    return manifest;
}

static oci_image_spec *load_image_config(const char *fname)
{
    oci_image_spec *config = NULL;
    parser_error err = NULL;

    if (fname == NULL) {
        return NULL;
    }

    config = oci_image_spec_parse_file(fname, NULL, &err);
    if (config == NULL) {
        ERROR("Parse image config file %s err:%s", fname, err);
    }

    free(err);
    return config;
}

static ssize_t load_image_archive_io_read(void *context, void *buf, size_t buf_len)
{
    int *read_fd = (int *)context;

    return util_read_nointr(*read_fd, buf, buf_len);
}

static int load_image_archive_io_close(void *context, char **err)
{
    int *read_fd = (int *)context;

    close(*read_fd);
    free(read_fd);
    return 0;
}

static int file_read_wrapper(const char *image_data_path, struct io_read_wrapper *reader)
{
    int ret = 0;
    int *fd_ptr = NULL;

    fd_ptr = util_common_calloc_s(sizeof(int));
    if (fd_ptr == NULL) {
        ERROR("Memory out");
        return -1;
    }

    *fd_ptr = util_open(image_data_path, O_RDONLY, 0);
    if (*fd_ptr == -1) {
        ERROR("Failed to open layer data %s", image_data_path);
        ret = -1;
        goto out;
    }

    reader->context = fd_ptr;
    fd_ptr = NULL;
    reader->read = load_image_archive_io_read;
    reader->close = load_image_archive_io_close;

out:
    free(fd_ptr);
    return ret;
}

static void oci_load_free_layer(load_layer_blob_t *l)
{
    if (l == NULL) {
        return;
    }

    if (l->chain_id != NULL) {
        free(l->chain_id);
        l->chain_id = NULL;
    }

    if (l->diff_id != NULL) {
        free(l->diff_id);
        l->diff_id = NULL;
    }

    if (l->compressed_digest != NULL) {
        free(l->compressed_digest);
        l->compressed_digest = NULL;
    }

    if (l->fpath != NULL) {
        free(l->fpath);
        l->fpath = NULL;
    }
    free(l);
}

static void oci_load_free_image(load_image_t *im)
{
    int i = 0;

    if (im == NULL) {
        return;
    }

    if (im->config_fpath != NULL) {
        free(im->config_fpath);
        im->config_fpath = NULL;
    }

    if (im->im_id != NULL) {
        free(im->im_id);
        im->im_id = NULL;
    }

    if (im->im_digest != NULL) {
        free(im->im_digest);
        im->im_digest = NULL;
    }

    if (im->manifest_fpath != NULL) {
        free(im->manifest_fpath);
        im->manifest_fpath = NULL;
    }

    if (im->manifest_digest != NULL) {
        free(im->manifest_digest);
        im->manifest_digest = NULL;
    }

    util_free_array_by_len(im->repo_tags, im->repo_tags_len);

    for (; i < im->layers_len; i++) {
        oci_load_free_layer(im->layers[i]);
    }
    free(im->layers);
    im->layers = NULL;

    free_oci_image_manifest(im->manifest);

    free(im->layer_of_hold_refs);
    im->layer_of_hold_refs = NULL;

    free(im);
}

inline static void do_free_load_image(load_image_t *im)
{
    if (im == NULL) {
        return;
    }

    if (im->layer_of_hold_refs != NULL && storage_dec_hold_refs(im->layer_of_hold_refs) != 0) {
        ERROR("decrease hold refs failed for layer %s", im->layer_of_hold_refs);
    }

    oci_load_free_image(im);
}

static char **str_array_copy(char **arr, size_t len)
{
    char **str_arr = NULL;
    size_t i = 0;

    str_arr = util_common_calloc_s(sizeof(char *) * len);
    if (str_arr == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    for (; i < len; i++) {
        str_arr[i] = util_strdup_s(arr[i]);
    }

    return str_arr;
}

static char *oci_load_calc_chain_id(char *parent_chain_id, char *diff_id)
{
    int sret = 0;
    char tmp_buffer[MAX_ID_BUF_LEN] = { 0 };
    char *digest = NULL;
    char *full_digest = NULL;

    if (parent_chain_id == NULL || diff_id == NULL) {
        ERROR("Invalid NULL param");
        return NULL;
    }

    if (strlen(diff_id) <= strlen(SHA256_PREFIX)) {
        ERROR("Invalid diff id %s found when calc chain id", diff_id);
        return NULL;
    }

    if (strlen(parent_chain_id) == 0) {
        return util_strdup_s(diff_id);
    }

    if (strlen(parent_chain_id) <= strlen(SHA256_PREFIX)) {
        ERROR("Invalid parent chain id %s found when calc chain id", parent_chain_id);
        return NULL;
    }

    sret = snprintf(tmp_buffer, sizeof(tmp_buffer), "%s+%s", parent_chain_id + strlen(SHA256_PREFIX),
                    diff_id + strlen(SHA256_PREFIX));
    if (sret < 0 || (size_t)sret >= sizeof(tmp_buffer)) {
        ERROR("Failed to sprintf chain id original string");
        return NULL;
    }

    digest = sha256_digest_str(tmp_buffer);
    if (digest == NULL) {
        ERROR("Failed to calculate chain id");
        goto out;
    }

    full_digest = util_full_digest(digest);

out:
    free(digest);
    return full_digest;
}

static char *oci_load_without_sha256_prefix(char *digest)
{
    if (digest == NULL) {
        ERROR("Invalid digest NULL when strip sha256 prefix");
        return NULL;
    }

    return digest + strlen(SHA256_PREFIX);
}

static int registry_layer_from_tarball(const load_layer_blob_t *layer, const char *id, const char *parent)
{
    int ret = 0;

    if (layer == NULL || id == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    storage_layer_create_opts_t copts = {
        .parent = parent,
        .uncompress_digest = layer->diff_id,
        .compressed_digest = layer->compressed_digest,
        .writable = false,
        .layer_data_path = layer->fpath,
    };

    if (storage_layer_create(id, &copts) != 0) {
        ERROR("create layer %s failed, parent %s, file %s", id, parent, layer->fpath);
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int oci_load_register_layers(load_image_t *desc)
{
    int ret = 0;
    size_t i = 0;
    char *id = NULL;
    char *parent = NULL;

    if (desc == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    if (desc->layers_len == 0) {
        ERROR("No layer found failed");
        return -1;
    }

    for (i = 0; i < desc->layers_len; i++) {
        id = oci_load_without_sha256_prefix(desc->layers[i]->chain_id);
        if (id == NULL) {
            ERROR("layer %zu have NULL digest for image %s", i, desc->im_id);
            ret = -1;
            goto out;
        }

        if (desc->layers[i]->alread_exist) {
            DEBUG("Layer:%s is already exist in storage, no need to registry", desc->layers[i]->fpath);
            parent = id;
            continue;
        }

        if (registry_layer_from_tarball(desc->layers[i], id, parent) != 0) {
            ERROR("Registry layer:%s from local tarball failed", desc->layers[i]->fpath);
            ret = -1;
            goto out;
        }

        free(desc->layer_of_hold_refs);
        desc->layer_of_hold_refs = util_strdup_s(id);
        if (parent != NULL && storage_dec_hold_refs(parent) != 0) {
            ERROR("decrease hold refs failed for layer %s", parent);
            ret = -1;
            goto out;
        }

        parent = id;
    }

out:
    return ret;
}

static int oci_load_set_image_name(const char *img_id, const char *img_name)
{
    int ret = 0;
    char *normalized_name = NULL;

    if (img_id == NULL || img_name == NULL) {
        ERROR("Invalid input arguments, image id or name is null, cannot set image name");
        return -1;
    }

    normalized_name = oci_normalize_image_name(img_name);
    if (normalized_name == NULL) {
        ret = -1;
        ERROR("Failed to normalized name %s", img_name);
        goto out;
    }

    if (storage_img_add_name(img_id, normalized_name) != 0) {
        ret = -1;
        ERROR("add image name failed");
    }

out:
    UTIL_FREE_AND_SET_NULL(normalized_name);
    return ret;
}

static int check_time_valid(oci_image_spec *conf)
{
    int i = 0;

    if (!oci_valid_time(conf->created)) {
        ERROR("Invalid created time %s", conf->created);
        return -1;
    }

    for (i = 0; i < conf->history_len; i++) {
        if (!oci_valid_time(conf->history[i]->created)) {
            ERROR("Invalid history created time %s", conf->history[i]->created);
            return -1;
        }
    }

    return 0;
}

static int oci_load_create_image(load_image_t *desc, const char *dst_tag)
{
    int ret = 0;
    size_t i = 0;
    size_t top_layer_index = 0;
    struct storage_img_create_options opts = { 0 };
    char *top_layer_id = NULL;
    char *pre_top_layer = NULL;
    oci_image_spec *conf = NULL;
    types_timestamp_t timestamp = { 0 };

    if (desc == NULL || desc->im_id == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    conf = load_image_config(desc->config_fpath);
    if (conf == NULL || conf->created == NULL) {
        ERROR("Get image created time failed");
        ret = -1;
        goto out;
    }

    ret = check_time_valid(conf);
    if (ret != 0) {
        goto out;
    }

    timestamp = util_to_timestamp_from_str(conf->created);
    top_layer_index = desc->layers_len - 1;
    opts.create_time = &timestamp;
    opts.digest = desc->manifest_digest;
    top_layer_id = oci_load_without_sha256_prefix(desc->layers[top_layer_index]->chain_id);
    if (top_layer_id == NULL) {
        ERROR("NULL top layer id found for image %s", desc->im_id);
        ret = -1;
        goto out;
    }

    if (storage_img_create(desc->im_id, top_layer_id, NULL, &opts) != 0) {
        pre_top_layer = storage_get_img_top_layer(desc->im_id);
        if (pre_top_layer == NULL) {
            ERROR("create image %s failed", desc->im_id);
            ret = -1;
            goto out;
        }

        if (strcmp(pre_top_layer, top_layer_id) != 0) {
            ERROR("error load image, image id %s exist, but top layer doesn't match. local %s, load %s", desc->im_id,
                  pre_top_layer, top_layer_id);
            ret = -1;
            goto out;
        }
    }

    if (dst_tag != NULL) {
        if (oci_load_set_image_name(desc->im_id, dst_tag) != 0) {
            ERROR("Failed to set image:%s name by using tag:%s", desc->im_id, dst_tag);
            ret = -1;
            goto out;
        }
    } else {
        for (; i < desc->repo_tags_len; i++) {
            if (oci_load_set_image_name(desc->im_id, desc->repo_tags[i]) != 0) {
                ERROR("Failed to set image:%s name by using tag:%s", desc->im_id, desc->repo_tags[i]);
                ret = -1;
                goto out;
            }
        }
    }

out:
    free_oci_image_spec(conf);
    free(pre_top_layer);
    return ret;
}

static int oci_load_set_manifest(const oci_image_manifest *m, char *image_id)
{
    int ret = 0;
    char *manifest_str = NULL;
    parser_error err = NULL;

    if (m == NULL || image_id == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    manifest_str = oci_image_manifest_generate_json(m, NULL, &err);
    if (manifest_str == NULL) {
        ERROR("Generate image %s manifest json err:%s", image_id, err);
        ret = -1;
        goto out;
    }

    if (storage_img_set_big_data(image_id, MANIFEST_BIG_DATA_KEY, manifest_str) != 0) {
        ERROR("set big data failed");
        ret = -1;
    }

out:
    free(err);
    free(manifest_str);
    return ret;
}

static int oci_load_set_config(load_image_t *desc)
{
    int ret = 0;
    char *config_str = NULL;

    if (desc == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    config_str = util_read_text_file(desc->config_fpath);
    if (config_str == NULL) {
        ERROR("read file %s content failed", desc->config_fpath);
        ret = -1;
        goto out;
    }

    if (storage_img_set_big_data(desc->im_id, desc->im_digest, config_str) != 0) {
        ERROR("set big data failed");
        ret = -1;
    }

out:
    free(config_str);
    config_str = NULL;
    return ret;
}

static int oci_load_set_loaded_time(char *image_id)
{
    int ret = 0;
    types_timestamp_t now = { 0 };

    if (!util_get_now_time_stamp(&now)) {
        ret = -1;
        ERROR("get now time stamp failed");
        goto out;
    }

    if (storage_img_set_loaded_time(image_id, &now) != 0) {
        ERROR("set loaded time failed");
        ret = -1;
    }

out:
    return ret;
}

static int oci_load_register_image(load_image_t *desc, const char *dst_tag)
{
    int ret = 0;
    bool image_created = false;

    if (desc == NULL || desc->im_id == NULL) {
        ERROR("Invalid NULL pointer");
        return -1;
    }

    if (oci_load_register_layers(desc) != 0) {
        ERROR("registry layers failed");
        ret = -1;
        goto out;
    }

    if (oci_load_create_image(desc, dst_tag) != 0) {
        ERROR("create image failed");
        ret = -1;
        goto out;
    }
    image_created = true;

    if (oci_load_set_config(desc) != 0) {
        ERROR("set image config failed");
        ret = -1;
        goto out;
    }

    if (oci_load_set_manifest(desc->manifest, desc->im_id) != 0) {
        ERROR("set manifest failed");
        ret = -1;
        goto out;
    }

    if (oci_load_set_loaded_time(desc->im_id) != 0) {
        ERROR("set loaded time failed");
        ret = -1;
        goto out;
    }

    if (storage_img_set_image_size(desc->im_id) != 0) {
        ERROR("set image size failed for %s failed", desc->im_id);
        ret = -1;
    }

out:
    if (ret != 0 && image_created) {
        if (storage_img_delete(desc->im_id, true)) {
            ERROR("delete image %s failed", desc->im_id);
        }
    }
    return ret;
}

static int check_and_set_digest_from_tarball(load_layer_blob_t *layer, const char *conf_diff_id)
{
    int ret = 0;
    bool gzip = false;

    if (layer == NULL || conf_diff_id == NULL) {
        ERROR("Invalid input param");
        return -1;
    }

    if (!util_file_exists(layer->fpath)) {
        ERROR("Layer data file:%s is not exist", layer->fpath);
        isulad_try_set_error_message("%s no such file", layer->fpath);
        ret = -1;
        goto out;
    }

    layer->alread_exist = false;
    layer->diff_id = oci_calc_diffid(layer->fpath);
    if (layer->diff_id == NULL) {
        ERROR("Calc layer:%s diff id failed", layer->fpath);
        ret = -1;
        goto out;
    }

    if (util_gzip_compressed(layer->fpath, &gzip) != 0) {
        ERROR("Judge layer file gzip attr err");
        ret = -1;
        goto out;
    }

    layer->compressed_digest = gzip ? sha256_full_file_digest(layer->fpath) : util_strdup_s(layer->diff_id);
    if (layer->compressed_digest == NULL) {
        ERROR("Calc layer %s compressed digest failed", layer->fpath);
        ret = -1;
        goto out;
    }

    if (strcmp(layer->diff_id, conf_diff_id) != 0) {
        ERROR("invalid diff id for layer:%s: expected %s, got %s", layer->chain_id, conf_diff_id, layer->diff_id);
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int oci_load_set_layers_info(load_image_t *im, const image_manifest_items_element *manifest, const char *dstdir)
{
    int ret = 0;
    size_t i = 0;
    oci_image_spec *conf = NULL;
    char *parent_chain_id_sha256 = "";
    char *id = NULL;
    char *parent_chain_id = NULL;

    if (im == NULL || manifest == NULL || dstdir == NULL) {
        ERROR("Invalid input params image or manifest is null");
        return -1;
    }

    im->layers_len = manifest->layers_len;
    im->layers = util_common_calloc_s(sizeof(load_layer_blob_t *) * manifest->layers_len);
    if (im->layers == NULL) {
        ERROR("Calloc memory failed");
        ret = -1;
        goto out;
    }

    conf = load_image_config(im->config_fpath);
    if (conf == NULL || conf->rootfs == NULL) {
        ERROR("Load image config file %s failed", im->config_fpath);
        ret = -1;
        goto out;
    }

    if (conf->rootfs->diff_ids_len != im->layers_len) {
        ERROR("Invalid manifest, layers length mismatch: expected %zu, got %zu", im->layers_len, conf->rootfs->diff_ids_len);
        ret = -1;
        goto out;
    }

    for (; i < conf->rootfs->diff_ids_len; i++) {
        im->layers[i] = util_common_calloc_s(sizeof(load_layer_blob_t));
        if (im->layers[i] == NULL) {
            ERROR("Out of memory");
            ret = -1;
            goto out;
        }

        im->layers[i]->fpath = util_path_join(dstdir, manifest->layers[i]);
        if (im->layers[i]->fpath == NULL) {
            ERROR("Path join failed");
            ret = -1;
            goto out;
        }
        // The format is sha256:xxx
        im->layers[i]->chain_id = oci_load_calc_chain_id(parent_chain_id_sha256, conf->rootfs->diff_ids[i]);
        if (im->layers[i]->chain_id == NULL) {
            ERROR("calc chain id failed, diff id %s, parent chain id %s", conf->rootfs->diff_ids[i], parent_chain_id_sha256);
            ret = -1;
            goto out;
        }
        parent_chain_id_sha256 = im->layers[i]->chain_id;

        id = oci_load_without_sha256_prefix(im->layers[i]->chain_id);
        if (id == NULL) {
            ERROR("Wipe out sha256 prefix failed from layer with chain id : %s", im->layers[i]->chain_id);
            ret = -1;
            goto out;
        }

        if (storage_inc_hold_refs(id) == 0) {
            free(im->layer_of_hold_refs);
            im->layer_of_hold_refs = util_strdup_s(id);
            if (parent_chain_id != NULL && storage_dec_hold_refs(parent_chain_id) != 0) {
                ERROR("Decrease hold refs failed for layer with chain id:%s", parent_chain_id);
                ret = -1;
                goto out;
            }

            im->layers[i]->diff_id = util_strdup_s(conf->rootfs->diff_ids[i]);
            if (im->layers[i]->diff_id == NULL) {
                ERROR("Dup layer diff id:%s from conf failed", conf->rootfs->diff_ids[i]);
                ret = -1;
                goto out;
            }
            im->layers[i]->alread_exist = true;
            parent_chain_id = id;
            continue;
        }

        if (check_and_set_digest_from_tarball(im->layers[i], conf->rootfs->diff_ids[i]) != 0) {
            ERROR("Check layer digest failed");
            ret = -1;
            goto out;
        }
    }

out:
    free_oci_image_spec(conf);
    return ret;
}

static load_image_t *oci_load_process_manifest(const image_manifest_items_element *manifest, const char *dstdir)
{
    int ret = 0;
    char *config_fpath = NULL;
    char *image_digest = NULL;
    char *image_id = NULL;
    load_image_t *im = NULL;

    im = util_common_calloc_s(sizeof(load_image_t));
    if (im == NULL) {
        ret = -1;
        ERROR("Out of memory");
        goto out;
    }

    config_fpath = util_path_join(dstdir, manifest->config);
    if (config_fpath == NULL) {
        ret = -1;
        ERROR("Path:%s join failed", manifest->config);
        goto out;
    }

    image_digest = sha256_full_file_digest(config_fpath);
    if (image_digest == NULL) {
        ret = -1;
        ERROR("Calc image config file %s digest err", manifest->config);
        goto out;
    }

    image_id = oci_load_without_sha256_prefix(image_digest);
    if (image_id == NULL) {
        ret = -1;
        ERROR("Remove sha256 prefix error from image digest %s", image_digest);
        goto out;
    }

    im->im_id = util_strdup_s(image_id);
    im->im_digest = util_strdup_s(image_digest);
    im->config_fpath = util_strdup_s(config_fpath);
    im->repo_tags_len = manifest->repo_tags_len;
    im->repo_tags = manifest->repo_tags_len == 0 ? NULL : str_array_copy(manifest->repo_tags, manifest->repo_tags_len);

    if (oci_load_set_layers_info(im, manifest, dstdir) != 0) {
        ret = -1;
        ERROR("Image load set layers info err");
        goto out;
    }

out:
    free(config_fpath);
    free(image_digest);
    if (ret != 0) {
        oci_load_free_image(im);
        return NULL;
    }
    return im;
}

static int64_t get_layer_size_from_storage(char *chain_id_pre)
{
    char *id = NULL;
    struct layer *l = NULL;
    int64_t size = 0;

    if (chain_id_pre == NULL) {
        ERROR("Invalid input param");
        return -1;
    }

    id = oci_load_without_sha256_prefix(chain_id_pre);
    if (id == NULL) {
        ERROR("Get chain id failed from value:%s", chain_id_pre);
        size = -1;
        goto out;
    }

    l = storage_layer_get(id);
    if (l == NULL) {
        ERROR("Layer with chain id:%s is not exist in store", id);
        size = -1;
        goto out;
    }

    size = l->compress_size;

out:
    free_layer(l);
    return size;
}

static int oci_load_set_manifest_info(load_image_t *im)
{
    int ret = 0;
    size_t i = 0;
    int64_t size = 0;

    if (im == NULL) {
        ERROR("Invalid input image ptr");
        return -1;
    }

    im->manifest = util_common_calloc_s(sizeof(oci_image_manifest));
    if (im->manifest == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    im->manifest->schema_version = OCI_SCHEMA_VERSION;
    im->manifest->layers = util_common_calloc_s(sizeof(oci_image_content_descriptor *) * im->layers_len);
    if (im->manifest->layers == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    im->manifest->layers_len = im->layers_len;
    im->manifest->config = util_common_calloc_s(sizeof(oci_image_content_descriptor));
    if (im->manifest->config == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    im->manifest->config->media_type = util_strdup_s(MediaTypeDockerSchema2Config);
    im->manifest->config->digest = util_strdup_s(im->im_digest);

    size = util_file_size(im->config_fpath);
    if (size < 0) {
        ERROR("Calc image config file %s size err", im->config_fpath);
        ret = -1;
        goto out;
    }

    im->manifest->config->size = size;

    for (; i < im->layers_len; i++) {
        im->manifest->layers[i] = util_common_calloc_s(sizeof(oci_image_content_descriptor));
        if (im->manifest->layers[i] == NULL) {
            ret = -1;
            ERROR("Out of memory");
            goto out;
        }

        im->manifest->layers[i]->media_type = util_strdup_s(MediaTypeDockerSchema2LayerGzip);
        im->manifest->layers[i]->digest = util_strdup_s(im->layers[i]->diff_id);

        if (im->layers[i]->alread_exist) {
            size = get_layer_size_from_storage(im->layers[i]->chain_id);
            if (size < 0) {
                ERROR("Get image layer:%s size error from local store", im->layers[i]->chain_id);
                ret = -1;
                goto out;
            }
        } else {
            size = util_file_size(im->layers[i]->fpath);
            if (size < 0) {
                ERROR("Calc image layer %s size error", im->layers[i]->fpath);
                ret = -1;
                goto out;
            }
        }
        im->manifest->layers[i]->size = size;
    }

out:
    if (ret != 0) {
        free_oci_image_manifest(im->manifest);
        im->manifest = NULL;
    }
    return ret;
}

static size_t oci_tag_count(image_manifest_items_element **manifest, size_t manifest_len)
{
    size_t cnt_tags = 0;
    size_t i = 0;

    for (; i < manifest_len; i++) {
        cnt_tags += manifest[i]->repo_tags_len;
    }

    return cnt_tags;
}

static bool oci_valid_repo_tags(image_manifest_items_element **manifest, size_t manifest_len)
{
    size_t i = 0;
    size_t j = 0;
    bool res = true;

    for (i = 0; i < manifest_len; i++) {
        for (j = 0; j < manifest[i]->repo_tags_len; j++) {
            if (!util_valid_image_name(manifest[i]->repo_tags[j])) {
                ERROR("Invalid image name %s", manifest[i]->repo_tags[j]);
                res = false;
                goto out;
            }
        }
    }

out:
    return res;
}

static bool oci_check_load_tags(image_manifest_items_element **manifest, size_t manifest_len, const char *dst_tag)
{
    size_t repo_tag_cnt = 0;
    bool res = true;

    repo_tag_cnt = oci_tag_count(manifest, manifest_len);
    if (dst_tag != NULL) {
        if (repo_tag_cnt > 1 || manifest_len > 1) {
            res = false;
            ERROR("Can not use --tag option because more than one image found in tar archive");
            isulad_try_set_error_message("Can not use --tag option because more than one image found in tar archive");
            goto out;
        }

        if (!util_valid_image_name(dst_tag)) {
            res = false;
            ERROR("Invalid image name %s", dst_tag);
            isulad_try_set_error_message("Invalid image name:%s", dst_tag);
            goto out;
        }
    } else if (!oci_valid_repo_tags(manifest, manifest_len)) {
        // Valid manifest RepoTags value
        res = false;
        ERROR("Contain invalid image name in tar archive");
        isulad_try_set_error_message("Contain invalid image name in tar archive");
    }

out:
    return res;
}

static char *oci_load_path_create()
{
    int ret = 0;
    int nret = 0;
    char *image_tmp_path = NULL;
    char tmp_dir[PATH_MAX] = { 0 };
    struct oci_image_module_data *oci_image_data = NULL;

    oci_image_data = get_oci_image_data();
    ret = makesure_isulad_tmpdir_perm_right(oci_image_data->root_dir);
    if (ret != 0) {
        ERROR("failed to make sure permission of image tmp work dir");
        goto out;
    }

    image_tmp_path = oci_get_isulad_tmpdir(oci_image_data->root_dir);
    if (image_tmp_path == NULL) {
        ERROR("failed to get image tmp work dir");
        ret = -1;
        goto out;
    }

    nret = snprintf(tmp_dir, PATH_MAX, "%s/oci-image-load-XXXXXX", image_tmp_path);
    if (nret < 0 || (size_t)nret >= sizeof(tmp_dir)) {
        ERROR("Path is too long");
        ret = -1;
        goto out;
    }

    if (mkdtemp(tmp_dir) == NULL) {
        ERROR("make temporary dir failed: %s", strerror(errno));
        isulad_try_set_error_message("make temporary dir failed: %s", strerror(errno));
        ret = -1;
        goto out;
    }

out:
    free(image_tmp_path);
    return ret == 0 ? util_strdup_s(tmp_dir) : NULL;
}

int oci_do_load(const im_load_request *request)
{
    int ret = 0;
    size_t i = 0;
    struct archive_options options = { 0 };
    struct io_read_wrapper reader = { 0 };
    char *manifest_fpath = NULL;
    image_manifest_items_element **manifest = NULL;
    size_t manifest_len = 0;
    load_image_t *im = NULL;
    char *digest = NULL;
    char *dstdir = NULL;
    char *err = NULL;

    if (request == NULL || request->file == NULL) {
        ERROR("Invalid input arguments, cannot load image");
        return -1;
    }

    dstdir = oci_load_path_create();
    if (dstdir == NULL) {
        ERROR("create temporary direcory failed");
        ret = -1;
        goto out;
    }

    if (file_read_wrapper(request->file, &reader) != 0) {
        ERROR("Failed to fill layer read wrapper");
        isulad_try_set_error_message("Failed to fill layer read wrapper");
        ret = -1;
        goto out;
    }

    options.whiteout_format = NONE_WHITEOUT_FORMATE;
    if (archive_unpack(&reader, dstdir, &options, &err) != 0) {
        ERROR("Failed to unpack to %s: %s", dstdir, err);
        isulad_try_set_error_message("Failed to unpack to %s: %s", dstdir, err);
        ret = -1;
        goto out;
    }

    manifest_fpath = util_path_join(dstdir, "manifest.json");
    if (manifest_fpath == NULL) {
        ERROR("Failed to join manifest.json path:%s", dstdir);
        isulad_try_set_error_message("Failed to join manifest.json path:%s", dstdir);
        ret = -1;
        goto out;
    }

    manifest = load_manifest(manifest_fpath, &manifest_len);
    if (manifest == NULL) {
        ERROR("Failed to load manifest.json file from path:%s", manifest_fpath);
        isulad_try_set_error_message("Failed to load manifest.json file from path:%s", manifest_fpath);
        ret = -1;
        goto out;
    }

    if (!oci_check_load_tags(manifest, manifest_len, request->tag)) {
        ERROR("Value of --tags or repo tags invalid");
        isulad_try_set_error_message("Value of --tags or repo tags invalid");
        ret = -1;
        goto out;
    }

    digest = sha256_full_file_digest(manifest_fpath);
    if (digest == NULL) {
        ret = -1;
        ERROR("calculate digest failed for manifest file %s", manifest_fpath);
        isulad_try_set_error_message("calculate digest failed for manifest file %s", manifest_fpath);
        goto out;
    }

    for (; i < manifest_len; i++) {
        im = oci_load_process_manifest(manifest[i], dstdir);
        if (im == NULL) {
            ret = -1;
            isulad_try_set_error_message("process manifest failed");
            goto out;
        }

        if (oci_load_set_manifest_info(im) != 0) {
            ERROR("Image %s set manifest info err", im->im_id);
            ret = -1;
            goto out;
        }

        im->manifest_digest = util_strdup_s(digest);
        if (oci_load_register_image(im, request->tag) != 0) {
            ERROR("error register image %s to store", im->im_id);
            isulad_try_set_error_message("error register image %s to store", im->im_id);
            ret = -1;
            goto out;
        }

        do_free_load_image(im);
        im = NULL;
    }

out:
    if (ret != 0) {
        isulad_set_error_message("Load image %s failed: %s", request->file, g_isulad_errmsg);
    }
    free(manifest_fpath);
    free(digest);
    for (i = 0; i < manifest_len; i++) {
        free_image_manifest_items_element(manifest[i]);
    }
    free(manifest);

    do_free_load_image(im);

    if (reader.close != NULL) {
        reader.close(reader.context, NULL);
    }

    if (util_recursive_rmdir(dstdir, 0)) {
        WARN("failed to remove directory %s", dstdir);
    }
    free(dstdir);
    free(err);
    return ret;
}
