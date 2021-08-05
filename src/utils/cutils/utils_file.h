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
 * Create: 2018-11-1
 * Description: provide container sha256 functions
 ********************************************************************************/

#ifndef UTILS_CUTILS_UTILS_FILE_H
#define UTILS_CUTILS_UTILS_FILE_H

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

struct dirent;

#ifdef __cplusplus
extern "C" {
#endif

bool util_dir_exists(const char *path);

bool util_fileself_exists(const char *f);

bool util_file_exists(const char *f);

int util_path_remove(const char *path);

ssize_t util_write_nointr(int fd, const void *buf, size_t count);

ssize_t util_write_nointr_in_total(int fd, const char *buf, size_t count);

ssize_t util_read_nointr(int fd, void *buf, size_t count);

int util_mkdir_p(const char *dir, mode_t mode);

int util_recursive_rmdir(const char *dirpath, int recursive_depth);

char *util_path_join(const char *dir, const char *file);

int util_ensure_path(char **confpath, const char *path);

int util_build_dir(const char *name);

char *util_human_size(uint64_t val);

char *util_human_size_decimal(int64_t val);

int util_open(const char *filename, int flags, mode_t mode);

FILE *util_fopen(const char *filename, const char *mode);

int util_gzip_compressed(const char *filename, bool *gzip);

char *util_path_dir(const char *path);

char *util_add_path(const char *path, const char *name);

char *util_read_text_file(const char *path);

int64_t util_file_size(const char *filename);

int util_list_all_subdir(const char *directory, char ***out);

int util_file2str(const char *filename, char *buf, size_t len);

char *look_path(const char *file, char **err);

int util_write_file(const char *fname, const char *content, size_t content_len, mode_t mode);

char *verify_file_and_get_real_path(const char *file);

int util_copy_file(const char *src_file, const char *dst_file, mode_t mode);

char *util_path_base(const char *path);

char *util_read_content_from_file(const char *path);

void util_calculate_dir_size(const char *dirpath, int recursive_depth, int64_t *total_size, int64_t *total_inode);

void utils_calculate_dir_size_without_hardlink(const char *dirpath, int64_t *total_size, int64_t *total_inode);

typedef bool (*subdir_callback_t)(const char *, const struct dirent *, void *context);

int util_scan_subdirs(const char *directory, subdir_callback_t cb, void *context);

int util_atomic_write_file(const char *fname, const char *content, size_t content_len, mode_t mode, bool sync);

typedef bool (*read_line_callback_t)(const char *, void *context);

int util_proc_file_line_by_line(FILE *fp, read_line_callback_t cb, void *context);

int util_set_file_group(const char *fname, const char *group);

// try to remove the path, path is file or dir
int util_recursive_remove_path(const char *path);

// list only current directory's entries, not list recursive subdirs
int util_list_all_entries(const char *directory, char ***out);

// note: 1. If xattrs not support in src or dst filesystem, this function will ignore copying xattrs
//          and will NOT error out.
//       2. If fifo or socket exist in source, this function will return failure.
int util_copy_dir_recursive(char *copy_dst, char *copy_src);

int set_file_owner_for_user_remap(const char *filename, const char *user_remap);

#ifdef __cplusplus
}
#endif

#endif // UTILS_CUTILS_UTILS_FILE_H
