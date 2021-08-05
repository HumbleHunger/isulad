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

#ifndef UTILS_CUTILS_UTILS_CONVERT_H
#define UTILS_CUTILS_UTILS_CONVERT_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int util_safe_u16(const char *numstr, uint16_t *converted);
int util_safe_int(const char *num_str, int *converted);
int util_safe_uint(const char *numstr, unsigned int *converted);
int util_safe_llong(const char *numstr, long long *converted);
int util_safe_strtod(const char *numstr, double *converted);
int util_str_to_bool(const char *boolstr, bool *converted);
int util_safe_uint64(const char *numstr, uint64_t *converted);
int util_parse_octal_uint32(const char *numstr, uint32_t *converted);
char *util_uint_to_string(long long unsigned int data);
char *util_int_to_string(long long int data);

#ifdef __cplusplus
}
#endif

#endif // UTILS_CUTILS_UTILS_CONVERT_H
