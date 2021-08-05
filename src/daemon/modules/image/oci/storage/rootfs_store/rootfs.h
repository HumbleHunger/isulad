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
 * Create: 2020-05-12
 * Description: provide containers function definition
 ******************************************************************************/
#ifndef DAEMON_MODULES_IMAGE_OCI_STORAGE_ROOTFS_STORE_ROOTFS_H
#define DAEMON_MODULES_IMAGE_OCI_STORAGE_ROOTFS_STORE_ROOTFS_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "isula_libutils/storage_rootfs.h"
#include "isula_libutils/log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _cntrootfs_t {
    storage_rootfs *srootfs;
    uint64_t refcnt;
} cntrootfs_t;

cntrootfs_t *new_rootfs(storage_rootfs *scntr);
void rootfs_ref_inc(cntrootfs_t *cntr);
void rootfs_ref_dec(cntrootfs_t *cntr);
void free_rootfs_t(cntrootfs_t *ptr);

#ifdef __cplusplus
}
#endif

#endif // DAEMON_MODULES_IMAGE_OCI_STORAGE_ROOTFS_STORE_ROOTFS_H
