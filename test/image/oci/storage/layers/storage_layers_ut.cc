/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: wujing
 * Create: 2020-03-30
 * Description: provide oci storage images unit test
 ******************************************************************************/
#include "layer_store.h"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <tuple>
#include <fstream>
#include <climits>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gtest/gtest.h>
#include "path.h"
#include "utils.h"
#include "storage.h"
#include "layer.h"
#include "driver_quota_mock.h"

using ::testing::Args;
using ::testing::ByRef;
using ::testing::SetArgPointee;
using ::testing::DoAll;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::NotNull;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::_;

std::string GetDirectory()
{
    char abs_path[PATH_MAX] { 0x00 };
    int ret = readlink("/proc/self/exe", abs_path, sizeof(abs_path));
    if (ret < 0 || static_cast<size_t>(ret) >= sizeof(abs_path)) {
        return "";
    }

    for (int i { ret }; i >= 0; --i) {
        if (abs_path[i] == '/') {
            abs_path[i + 1] = '\0';
            break;
        }
    }

    return static_cast<std::string>(abs_path) + "../../../../../../test/image/oci/storage/layers";
}

bool dirExists(const char *path)
{
    DIR *dp = nullptr;
    if ((dp = opendir(path)) == nullptr) {
        return false;
    }

    closedir(dp);
    return true;
}

void free_layer(struct layer *ptr)
{
    if (ptr == nullptr) {
        return;
    }
    free(ptr->id);
    ptr->id = nullptr;
    free(ptr->parent);
    ptr->parent = nullptr;
    free(ptr->mount_point);
    ptr->mount_point = nullptr;
    free(ptr->compressed_digest);
    ptr->compressed_digest = nullptr;
    free(ptr->uncompressed_digest);
    ptr->uncompressed_digest = nullptr;
    free(ptr);
}

void free_layer_list(struct layer_list *ptr)
{
    size_t i = 0;
    if (ptr == nullptr) {
        return;
    }

    for (; i < ptr->layers_len; i++) {
        free_layer(ptr->layers[i]);
        ptr->layers[i] = nullptr;
    }
    free(ptr->layers);
    ptr->layers = nullptr;
    free(ptr);
}

int invokeIOCtl(int fd, int cmd)
{
    return 0;
}

int invokeQuotaCtl(int cmd, const char* special, int id, caddr_t addr)
{
#define XFS_QUOTA_PDQ_ACCT (1<<4)   // project quota accounting
#define XFS_QUOTA_PDQ_ENFD (1<<5)   // project quota limits enforcement

    fs_quota_stat_t* fs_quota_stat_info = (fs_quota_stat_t *)addr;
    fs_quota_stat_info->qs_flags = XFS_QUOTA_PDQ_ACCT | XFS_QUOTA_PDQ_ENFD;

    return 0;
}

/********************************test data 1: container layer json**************************************
{
    "id": "7db8f44a0a8e12ea4283e3180e98880007efbd5de2e7c98b67de9cdd4dfffb0b",
    "parent": "9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63",
    "created": "2020-07-09T16:54:43.402330834Z"
}

mount info
{
    "path": "/var/lib/isulad/storage/overlay/7db8f44a0a8e12ea4283e3180e98880007efbd5de2e7c98b67de9cdd4dfffb0b/merged"
}
 ******************************************************************************************/

/********************************test data 2: hello-world image layer json**************************************
{
    "id": "9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63",
    "names": [
        "hello_world:latest"
    ],
    "created": "2020-07-09T11:57:39.992267244Z",
    "compressed-diff-digest": "sha256:0e03bdcc26d7a9a57ef3b6f1bf1a210cff6239bff7c8cac72435984032851689",
    "diff-digest": "sha256:9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63",
    "diff-size": 1672256
}
 ******************************************************************************************/

class StorageLayersUnitTest : public testing::Test {
protected:
    void SetUp() override
    {
        MockDriverQuota_SetMock(&m_driver_quota_mock);
        struct storage_module_init_options opts = {0};

        std::string isulad_dir = "/var/lib/isulad/";
        std::string root_dir = isulad_dir + "data";
        std::string run_dir = isulad_dir + "data/run";
        std::string data_dir = GetDirectory() + "/data";

        ASSERT_STRNE(util_clean_path(data_dir.c_str(), data_path, sizeof(data_path)), nullptr);
        std::string cp_command = "cp -r " + std::string(data_path) + " " + isulad_dir;
        ASSERT_EQ(system(cp_command.c_str()), 0);

        ASSERT_STRNE(util_clean_path(root_dir.c_str(), real_path, sizeof(real_path)), nullptr);
        opts.storage_root = strdup(real_path);
        ASSERT_STRNE(util_clean_path(run_dir.c_str(), real_run_path, sizeof(real_run_path)), nullptr);
        opts.storage_run_root = strdup(real_run_path);
        opts.driver_name = strdup("overlay");

        EXPECT_CALL(m_driver_quota_mock, QuotaCtl(_, _, _, _)).WillRepeatedly(Invoke(invokeQuotaCtl));
        ASSERT_EQ(layer_store_init(&opts), 0);

        free(opts.storage_root);
        free(opts.storage_run_root);
        free(opts.driver_name);
    }

    void TearDown() override
    {
        MockDriverQuota_SetMock(nullptr);

        layer_store_exit();
        layer_store_cleanup();

        std::string rm_command = "rm -rf /var/lib/isulad/data";
        ASSERT_EQ(system(rm_command.c_str()), 0);
    }

    NiceMock<MockDriverQuota> m_driver_quota_mock;
    char real_path[PATH_MAX] = { 0x00 };
    char real_run_path[PATH_MAX] = { 0x00 };
    char data_path[PATH_MAX] = { 0x00 };
};

TEST_F(StorageLayersUnitTest, test_layers_load)
{
    struct layer_list *layer_list = (struct layer_list *)util_common_calloc_s(sizeof(struct layer_list));
    ASSERT_NE(layer_list, nullptr);

    ASSERT_EQ(layer_store_list(layer_list), 0);
    ASSERT_EQ(layer_list->layers_len, 2);

    struct layer **layers = layer_list->layers;
    ASSERT_NE(layers, nullptr);

    int id_container = 1;
    int id_image = 0;
    if (strcmp(layers[0]->id, "7db8f44a0a8e12ea4283e3180e98880007efbd5de2e7c98b67de9cdd4dfffb0b") == 0) {
        id_container = 0;
        id_image = 1;
    }

    // check layer 7db8f44a0a8e12ea4283e3180e98880007efbd5de2e7c98b67de9cdd4dfffb0b
    std::string mount_point = std::string(real_path) +
                              "/overlay/7db8f44a0a8e12ea4283e3180e98880007efbd5de2e7c98b67de9cdd4dfffb0b/merged";
    ASSERT_NE(layers[id_container], nullptr);
    ASSERT_STREQ(layers[id_container]->id, "7db8f44a0a8e12ea4283e3180e98880007efbd5de2e7c98b67de9cdd4dfffb0b");
    ASSERT_STREQ(layers[id_container]->parent, "9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63");
    ASSERT_STREQ(layers[id_container]->mount_point, mount_point.c_str());

    // check layer 9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63
    ASSERT_NE(layers[id_image], nullptr);
    ASSERT_STREQ(layers[id_image]->id, "9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63");
    ASSERT_STREQ(layers[id_image]->parent, nullptr);
    ASSERT_STREQ(layers[id_image]->compressed_digest,
                 "sha256:0e03bdcc26d7a9a57ef3b6f1bf1a210cff6239bff7c8cac72435984032851689");
    ASSERT_STREQ(layers[id_image]->uncompressed_digest,
                 "sha256:9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63");
    ASSERT_EQ(layers[id_image]->uncompress_size, 1672256);

    free_layer_list(layer_list);

    layer_list = (struct layer_list *)util_common_calloc_s(sizeof(struct layer_list));
    remove_layer_list_tail();
    ASSERT_EQ(layer_store_list(layer_list), 0);
    ASSERT_EQ(layer_list->layers_len, 1);
    free_layer_list(layer_list);
}

TEST_F(StorageLayersUnitTest, test_layer_store_exists)
{
    std::string id { "7db8f44a0a8e12ea4283e3180e98880007efbd5de2e7c98b67de9cdd4dfffb0b" };
    std::string incorrectId { "50551ff67da98ab8540d7132" };

    ASSERT_TRUE(layer_store_exists(id.c_str()));
    ASSERT_FALSE(layer_store_exists(incorrectId.c_str()));
}

TEST_F(StorageLayersUnitTest, test_layer_store_create)
{
    struct layer_opts *layer_opt = (struct layer_opts *)util_common_calloc_s(sizeof(struct layer_opts));
    layer_opt->parent = strdup("9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63");
    layer_opt->writable = true;

    layer_opt->opts = (struct layer_store_mount_opts *)util_common_calloc_s(sizeof(struct layer_store_mount_opts));
    layer_opt->opts->mount_opts = (json_map_string_string *)util_common_calloc_s(sizeof(json_map_string_string));
    layer_opt->opts->mount_opts->keys = (char **)util_common_calloc_s(sizeof(char *));
    layer_opt->opts->mount_opts->values = (char **)util_common_calloc_s(sizeof(char *));
    layer_opt->opts->mount_opts->keys[0] = strdup("size");
    layer_opt->opts->mount_opts->values[0] = strdup("128M");
    layer_opt->opts->mount_opts->len = 1;

    layer_opt->names = (char **)util_common_calloc_s(sizeof(char *));
    layer_opt->names[0] = strdup("layer_name");
    layer_opt->names_len = 1;

    EXPECT_CALL(m_driver_quota_mock, IOCtl(_, _)).WillRepeatedly(Invoke(invokeIOCtl));

    free_layer_opts(layer_opt);
}

TEST_F(StorageLayersUnitTest, test_layer_store_by_compress_digest)
{
    std::string compress { "sha256:0e03bdcc26d7a9a57ef3b6f1bf1a210cff6239bff7c8cac72435984032851689" };
    std::string id { "9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63" };
    struct layer_list *layer_list = (struct layer_list *)util_common_calloc_s(sizeof(struct layer_list));

    ASSERT_EQ(layer_store_by_compress_digest(compress.c_str(), layer_list), 0);
    ASSERT_EQ(layer_list->layers_len, 1);

    struct layer **layers = layer_list->layers;
    ASSERT_STREQ(layers[0]->id, id.c_str());
    ASSERT_STREQ(layers[0]->compressed_digest, compress.c_str());

    free_layer_list(layer_list);
}

TEST_F(StorageLayersUnitTest, test_layer_store_by_uncompress_digest)
{
    std::string uncompress { "sha256:9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63" };
    std::string id { "9c27e219663c25e0f28493790cc0b88bc973ba3b1686355f221c38a36978ac63" };
    struct layer_list *layer_list = (struct layer_list *)util_common_calloc_s(sizeof(struct layer_list));

    ASSERT_EQ(layer_store_by_uncompress_digest(uncompress.c_str(), layer_list), 0);
    ASSERT_EQ(layer_list->layers_len, 1);

    struct layer **layers = layer_list->layers;
    ASSERT_STREQ(layers[0]->id, id.c_str());
    ASSERT_STREQ(layers[0]->uncompressed_digest, uncompress.c_str());
    ASSERT_EQ(layers[0]->uncompress_size, 1672256);

    free_layer_list(layer_list);
}
