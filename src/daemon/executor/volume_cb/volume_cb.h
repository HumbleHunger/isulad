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
 * Author: wangfengtu
 * Create: 2020-09-03
 * Description: provide volume function definition
 ********************************************************************************/

#ifndef DAEMON_EXECUTOR_VOLUME_CB_VOLUME_CB_H
#define DAEMON_EXECUTOR_VOLUME_CB_VOLUME_CB_H

#include "callback.h"

#ifdef __cplusplus
extern "C" {
#endif

void volume_callback_init(service_volume_callback_t *cb);

#ifdef __cplusplus
}
#endif

#endif

