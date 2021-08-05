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
 * Author: tanyifeng
 * Create: 2018-11-08
 * Description: provide container path function definition
 ******************************************************************************/
#ifndef UTILS_CUTILS_PATH_H
#define UTILS_CUTILS_PATH_H

#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/*
 * util_clean_path is similar to realpath of glibc, but not expands symbolic links,
 * and not check the existence of components of the path.
 */
char *util_clean_path(const char *path, char *realpath, size_t realpath_len);

bool util_specify_current_dir(const char *path);

char *util_follow_symlink_in_scope(const char *fullpath, const char *rootpath);

int util_split_dir_and_base_name(const char *path, char **dir, char **base);

int util_filepath_split(const char *path, char **dir, char **base);

char *util_get_resource_path(const char *rootpath, const char *path);

int util_resolve_path(const char *rootpath, const char *path, char **resolvedpath, char **abspath);

bool util_has_trailing_path_separator(const char *path);

char *util_preserve_trailing_dot_or_separator(const char *cleanedpath, const char *originalpath);

int util_split_path_dir_entry(const char *path, char **dir, char **base);

int util_realpath_in_scope(const char *rootfs, const char *path, char **real_path);

#ifdef __cplusplus
}
#endif

#endif
