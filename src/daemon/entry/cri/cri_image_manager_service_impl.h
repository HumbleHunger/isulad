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
 * Description: provide cri image service function definition
 *********************************************************************************/

#ifndef DAEMON_ENTRY_CRI_CRI_IMAGE_SERVICE_H
#define DAEMON_ENTRY_CRI_CRI_IMAGE_SERVICE_H

#include <string>
#include <vector>
#include <memory>
// #include "cri_services.h"
#include "image_api.h"
#include "cri_image_manager_service.h"

namespace CRI {
class ImageManagerServiceImpl : public ImageManagerService {
public:
    ImageManagerServiceImpl() = default;
    virtual ~ImageManagerServiceImpl() = default;

    void ListImages(const runtime::v1alpha2::ImageFilter &filter,
                    std::vector<std::unique_ptr<runtime::v1alpha2::Image>> *images, Errors &error) override;

    std::unique_ptr<runtime::v1alpha2::Image> ImageStatus(const runtime::v1alpha2::ImageSpec &image,
                                                          Errors &error) override;

    std::string PullImage(const runtime::v1alpha2::ImageSpec &image, const runtime::v1alpha2::AuthConfig &auth,
                          Errors &error) override;

    void RemoveImage(const runtime::v1alpha2::ImageSpec &image, Errors &error) override;

    void ImageFsInfo(std::vector<std::unique_ptr<runtime::v1alpha2::FilesystemUsage>> *usages, Errors &error) override;

private:
    int pull_request_from_grpc(const runtime::v1alpha2::ImageSpec *image, const runtime::v1alpha2::AuthConfig *auth,
                               im_pull_request **request, Errors &error);

    int list_request_from_grpc(const runtime::v1alpha2::ImageFilter *filter, im_list_request **request, Errors &error);

    void list_images_to_grpc(im_list_response *response, std::vector<std::unique_ptr<runtime::v1alpha2::Image>> *images,
                             Errors &error);

    int status_request_from_grpc(const runtime::v1alpha2::ImageSpec *image, im_summary_request **request,
                                 Errors &error);

    std::unique_ptr<runtime::v1alpha2::Image> status_image_to_grpc(im_summary_response *response, Errors &error);

    void fs_info_to_grpc(im_fs_info_response *response,
                         std::vector<std::unique_ptr<runtime::v1alpha2::FilesystemUsage>> *fs_infos, Errors &error);

    int remove_request_from_grpc(const runtime::v1alpha2::ImageSpec *image, im_rmi_request **request, Errors &error);
};

} // namespace CRI
#endif // DAEMON_ENTRY_CRI_CRI_IMAGE_SERVICE_H
