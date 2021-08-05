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
 * Description: provide container snapshot functions definition
 *******************************************************************************/

#ifndef __SNAPSHOT_H
#define __SNAPSHOT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "db_all.h"

int snapshot_init(uint32_t driver_type);

int snapshot_generate_mount_string(uint32_t driver_type,
                                   struct db_image *imginfo,
                                   struct db_sninfo **sninfos, char **mount_string);

#endif

