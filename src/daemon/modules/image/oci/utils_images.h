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
 * Author: WuJing
 * Create: 2020-05-09
 * Description: provide isula image common functions
 ********************************************************************************/

#ifndef DAEMON_MODULES_IMAGE_OCI_UTILS_IMAGES_H
#define DAEMON_MODULES_IMAGE_OCI_UTILS_IMAGES_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>

#include "isula_libutils/docker_image_config_v2.h"
#include "isula_libutils/registry_manifest_schema1.h"
#include "isula_libutils/image_manifest_v1_compatibility.h"
#include "registry_type.h"
#include "utils_timestamp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HTTPS_PREFIX "https://"
#define HTTP_PREFIX "http://"

#define DEFAULT_TAG ":latest"
#define HOSTNAME_TO_STRIP "docker.io/"
#define REPO_PREFIX_TO_STRIP "library/"
#define MAX_ID_BUF_LEN 256

char *oci_get_host(const char *name);
char *oci_host_from_mirror(const char *mirror);
char *oci_default_tag(const char *name);
char *oci_add_host(const char *domain, const char *name);
char *oci_normalize_image_name(const char *name);
int oci_split_image_name(const char *image_name, char **host, char **name, char **tag);
char *oci_strip_dockerio_prefix(const char *name);
char *make_big_data_base_name(const char *key);
char *oci_calc_diffid(const char *file);
void free_items_not_inherit(docker_image_config_v2 *config);
int add_rootfs_and_history(const layer_blob *layers, size_t layers_len, const registry_manifest_schema1 *manifest,
                           docker_image_config_v2 *config);
bool oci_valid_time(char *time);

char *oci_get_isulad_tmpdir(const char *root_dir);
int makesure_isulad_tmpdir_perm_right(const char *root_dir);

#ifdef __cplusplus
}
#endif

#endif // DAEMON_MODULES_IMAGE_OCI_UTILS_IMAGES_H
