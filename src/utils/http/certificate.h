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
 * Author: lifeng
 * Create: 2019-06-07
 * Description: provide certificate function
 ******************************************************************************/
#ifndef UTILS_HTTP_CERTIFICATE_H
#define UTILS_HTTP_CERTIFICATE_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* signature algorithm name max length */
#define  ALGO_NAME_LEN       256

/* public key bits suggest min length */
#define  RSA_PKEY_MIN_LEN       2048
#define  ECC_PKEY_MIN_LEN       256

int get_common_name_from_tls_cert(const char *cert_path, char *value, size_t len);

#ifdef __cplusplus
}
#endif

#endif

