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
 * Author: lifeng
 * Create: 2020-06-11
 * Description: provide namespace spec definition
 ******************************************************************************/
#ifndef DAEMON_MODULES_SPEC_SPECS_NAMESPACE_H
#define DAEMON_MODULES_SPEC_SPECS_NAMESPACE_H

#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

int get_share_namespace_path(const char *type, const char *src_path, char **dest_path);
char *get_container_process_label(const char *path);

#ifdef __cplusplus
}
#endif

#endif
