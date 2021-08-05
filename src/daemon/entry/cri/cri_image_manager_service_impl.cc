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
 * Author: tanyifeng
 * Create: 2017-11-22
 * Description: provide cri image functions
 *********************************************************************************/
#include "cri_image_manager_service_impl.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <grpc++/grpc++.h>
#include <unistd.h>

#include "cri_helpers.h"
#include "err_msg.h"
#include "events_sender_api.h"
#include "isula_libutils/log.h"
#include "service_image_api.h"
#include "utils.h"

using namespace CRI;
static void conv_image_to_grpc(const imagetool_image_summary *element, std::unique_ptr<runtime::v1alpha2::Image> &image)
{
    if (element == nullptr) {
        return;
    }

    if (element->id != nullptr) {
        image->set_id(element->id);
    }

    for (size_t j = 0; j < element->repo_tags_len; j++) {
        if (element->repo_tags[j] != nullptr) {
            image->add_repo_tags(element->repo_tags[j]);
        }
    }

    for (size_t j = 0; j < element->repo_digests_len; j++) {
        if (element->repo_digests[j] != nullptr) {
            image->add_repo_digests(element->repo_digests[j]);
        }
    }

    image->set_size(element->size);

    if (element->uid != nullptr) {
        runtime::v1alpha2::Int64Value *uid_value = new (std::nothrow) runtime::v1alpha2::Int64Value;
        if (uid_value == nullptr) {
            return;
        }
        uid_value->set_value(element->uid->value);
        image->set_allocated_uid(uid_value);
    }

    if (element->username != nullptr) {
        image->set_username(element->username);
    }
}

auto ImageManagerServiceImpl::pull_request_from_grpc(const runtime::v1alpha2::ImageSpec *image,
                                                     const runtime::v1alpha2::AuthConfig *auth, im_pull_request **request,
                                                     Errors &error) -> int
{
    im_pull_request *tmpreq = (im_pull_request *)util_common_calloc_s(sizeof(im_pull_request));
    if (tmpreq == nullptr) {
        ERROR("Out of memory");
        error.SetError("Out of memory");
        return -1;
    }

    if (!image->image().empty()) {
        tmpreq->image = util_strdup_s(image->image().c_str());
    }

    if (!auth->username().empty()) {
        tmpreq->username = util_strdup_s(auth->username().c_str());
    }

    if (!auth->password().empty()) {
        tmpreq->password = util_strdup_s(auth->password().c_str());
    }

    if (!auth->auth().empty()) {
        tmpreq->auth = util_strdup_s(auth->auth().c_str());
    }

    if (!auth->server_address().empty()) {
        tmpreq->server_address = util_strdup_s(auth->server_address().c_str());
    }

    if (!auth->identity_token().empty()) {
        tmpreq->identity_token = util_strdup_s(auth->identity_token().c_str());
    }

    if (!auth->registry_token().empty()) {
        tmpreq->registry_token = util_strdup_s(auth->registry_token().c_str());
    }

    *request = tmpreq;

    return 0;
}

auto ImageManagerServiceImpl::list_request_from_grpc(const runtime::v1alpha2::ImageFilter *filter,
                                                     im_list_request **request, Errors &error) -> int
{
    im_list_request *tmpreq = (im_list_request *)util_common_calloc_s(sizeof(im_list_request));
    if (tmpreq == nullptr) {
        ERROR("Out of memory");
        error.SetError("Out of memory");
        return -1;
    }

    if (!filter->image().image().empty()) {
        tmpreq->filter.image.image = util_strdup_s(filter->image().image().c_str());
    }

    *request = tmpreq;

    return 0;
}

void ImageManagerServiceImpl::list_images_to_grpc(im_list_response *response,
                                                  std::vector<std::unique_ptr<runtime::v1alpha2::Image>> *images,
                                                  Errors &error)
{
    imagetool_images_list *list_images = response->images;
    if (list_images == nullptr) {
        return;
    }

    for (size_t i = 0; i < list_images->images_len; i++) {
        std::unique_ptr<runtime::v1alpha2::Image> image(new (std::nothrow) runtime::v1alpha2::Image);
        if (image == nullptr) {
            error.SetError("Out of memory");
            return;
        }

        imagetool_image_summary *element = list_images->images[i];
        conv_image_to_grpc(element, image);
        images->push_back(move(image));
    }
}

void ImageManagerServiceImpl::ListImages(const runtime::v1alpha2::ImageFilter &filter,
                                         std::vector<std::unique_ptr<runtime::v1alpha2::Image>> *images, Errors &error)
{
    im_list_request *request { nullptr };
    im_list_response *response { nullptr };

    int ret = list_request_from_grpc(&filter, &request, error);
    if (ret != 0) {
        goto cleanup;
    }

    ret = im_list_images(request, &response);
    if (ret != 0) {
        if (response != nullptr && response->errmsg != nullptr) {
            error.SetError(response->errmsg);
        } else {
            error.SetError("Failed to call pull image");
        }
        goto cleanup;
    }

    list_images_to_grpc(response, images, error);

cleanup:
    DAEMON_CLEAR_ERRMSG();
    free_im_list_request(request);
    free_im_list_response(response);
}

auto ImageManagerServiceImpl::status_request_from_grpc(const runtime::v1alpha2::ImageSpec *image,
                                                       im_summary_request **request, Errors &error) -> int
{
    im_summary_request *tmpreq = (im_summary_request *)util_common_calloc_s(sizeof(im_summary_request));
    if (tmpreq == nullptr) {
        ERROR("Out of memory");
        error.SetError("Out of memory");
        return -1;
    }

    if (!image->image().empty()) {
        tmpreq->image.image = util_strdup_s(image->image().c_str());
    }

    *request = tmpreq;

    return 0;
}

std::unique_ptr<runtime::v1alpha2::Image> ImageManagerServiceImpl::status_image_to_grpc(im_summary_response *response,
                                                                                        Errors & /*error*/)
{
    imagetool_image_summary *image_info = response->image_summary;
    if (image_info == nullptr) {
        return nullptr;
    }

    std::unique_ptr<runtime::v1alpha2::Image> image(new (std::nothrow) runtime::v1alpha2::Image);
    if (image == nullptr) {
        ERROR("Out of memory");
        return nullptr;
    }
    conv_image_to_grpc(image_info, image);

    return image;
}

std::unique_ptr<runtime::v1alpha2::Image> ImageManagerServiceImpl::ImageStatus(const runtime::v1alpha2::ImageSpec
                                                                               &image,
                                                                               Errors &error)
{
    im_summary_request *request { nullptr };
    im_summary_response *response { nullptr };
    std::unique_ptr<runtime::v1alpha2::Image> out { nullptr };

    int ret = status_request_from_grpc(&image, &request, error);
    if (ret != 0) {
        goto cleanup;
    }

    ret = im_image_summary(request, &response);
    if (ret != 0) {
        if (response != nullptr && response->errmsg != nullptr) {
            error.SetError(response->errmsg);
        } else {
            error.SetError("Failed to call status image");
        }
        goto cleanup;
    }

    out = status_image_to_grpc(response, error);

cleanup:
    DAEMON_CLEAR_ERRMSG();
    free_im_summary_request(request);
    free_im_summary_response(response);
    return out;
}

auto ImageManagerServiceImpl::PullImage(const runtime::v1alpha2::ImageSpec &image,
                                        const runtime::v1alpha2::AuthConfig &auth, Errors &error) -> std::string
{
    std::string out_str;
    im_pull_request *request { nullptr };
    im_pull_response *response { nullptr };

    int ret = pull_request_from_grpc(&image, &auth, &request, error);
    if (ret != 0) {
        goto cleanup;
    }
    request->type = util_strdup_s(IMAGE_TYPE_OCI);

    ret = im_pull_image(request, &response);
    if (ret != 0) {
        if (response != nullptr && response->errmsg != nullptr) {
            error.SetError(response->errmsg);
        } else {
            error.SetError("Failed to call pull image");
        }
        goto cleanup;
    }
    if (response->image_ref != nullptr) {
        out_str = response->image_ref;
    }
    (void)isulad_monitor_send_image_event(request->image, IM_PULL);

cleanup:
    DAEMON_CLEAR_ERRMSG();
    free_im_pull_request(request);
    free_im_pull_response(response);
    return out_str;
}

auto ImageManagerServiceImpl::remove_request_from_grpc(const runtime::v1alpha2::ImageSpec *image,
                                                       im_rmi_request **request,
                                                       Errors &error) -> int
{
    im_rmi_request *tmpreq = (im_rmi_request *)util_common_calloc_s(sizeof(im_rmi_request));
    if (tmpreq == nullptr) {
        ERROR("Out of memory");
        error.SetError("Out of memory");
        return -1;
    }

    if (!image->image().empty()) {
        tmpreq->image.image = util_strdup_s(image->image().c_str());
    }

    *request = tmpreq;

    return 0;
}

void ImageManagerServiceImpl::RemoveImage(const runtime::v1alpha2::ImageSpec &image, Errors &error)
{
    std::string out_str;
    im_rmi_request *request { nullptr };

    DAEMON_CLEAR_ERRMSG();

    if (remove_request_from_grpc(&image, &request, error) != 0) {
        goto cleanup;
    }

    if (delete_image(request->image.image, false) != 0) {
        if (g_isulad_errmsg != nullptr) {
            error.SetError(g_isulad_errmsg);
        } else {
            error.SetError("Failed to call remove image");
        }
    }

cleanup:
    DAEMON_CLEAR_ERRMSG();
    free_im_remove_request(request);
}

void ImageManagerServiceImpl::fs_info_to_grpc(im_fs_info_response *response,
                                              std::vector<std::unique_ptr<runtime::v1alpha2::FilesystemUsage>> *fs_infos,
                                              Errors & /*error*/)
{
    imagetool_fs_info *got_fs_info = response->fs_info;
    if (got_fs_info == nullptr) {
        return;
    }

    for (size_t i {}; i < got_fs_info->image_filesystems_len; i++) {
        using FilesystemUsagePtr = std::unique_ptr<runtime::v1alpha2::FilesystemUsage>;
        FilesystemUsagePtr fs_info(new (std::nothrow) runtime::v1alpha2::FilesystemUsage);
        if (fs_info == nullptr) {
            ERROR("Out of memory");
            return;
        }

        imagetool_fs_info_image_filesystems_element *element = got_fs_info->image_filesystems[i];

        fs_info->set_timestamp(element->timestamp);

        if (element->fs_id != nullptr && element->fs_id->mountpoint != nullptr) {
            runtime::v1alpha2::FilesystemIdentifier *fs_id = new (std::nothrow) runtime::v1alpha2::FilesystemIdentifier;
            if (fs_id == nullptr) {
                ERROR("Out of memory");
                return;
            }
            fs_id->set_mountpoint(element->fs_id->mountpoint);
            fs_info->set_allocated_fs_id(fs_id);
        }

        if (element->used_bytes != nullptr) {
            runtime::v1alpha2::UInt64Value *used_bytes = new (std::nothrow) runtime::v1alpha2::UInt64Value;
            if (used_bytes == nullptr) {
                ERROR("Out of memory");
                return;
            }
            used_bytes->set_value(element->used_bytes->value);
            fs_info->set_allocated_used_bytes(used_bytes);
        }

        if (element->inodes_used != nullptr) {
            runtime::v1alpha2::UInt64Value *inodes_used = new (std::nothrow) runtime::v1alpha2::UInt64Value;
            if (inodes_used == nullptr) {
                ERROR("Out of memory");
                return;
            }
            inodes_used->set_value(element->inodes_used->value);
            fs_info->set_allocated_inodes_used(inodes_used);
        }

        fs_infos->push_back(std::move(fs_info));
    }
}

void ImageManagerServiceImpl::ImageFsInfo(std::vector<std::unique_ptr<runtime::v1alpha2::FilesystemUsage>> *usages,
                                          Errors &error)
{
    im_fs_info_response *response { nullptr };

    if (im_get_filesystem_info(IMAGE_TYPE_OCI, &response) != 0) {
        if (response != nullptr && response->errmsg != nullptr) {
            error.SetError(response->errmsg);
        } else {
            error.SetError("Failed to call inspect image filesystem info");
        }
        goto out;
    }

    fs_info_to_grpc(response, usages, error);

out:
    DAEMON_CLEAR_ERRMSG();
    free_im_fs_info_response(response);
}
