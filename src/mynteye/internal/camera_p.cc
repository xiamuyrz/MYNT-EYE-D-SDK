// Copyright 2018 Slightech Co., Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "mynteye/internal/camera_p.h"

#include <stdexcept>
#include <string>
#include <chrono>
#include <utility>

#include "mynteye/data/channels.h"
#include "mynteye/device/device.h"
#include "mynteye/internal/image_utils.h"
#include "mynteye/internal/motions.h"
#include "mynteye/internal/streams.h"
#include "mynteye/util/log.h"

MYNTEYE_USE_NAMESPACE

CameraPrivate::CameraPrivate() : device_(std::make_shared<Device>()) {
  DBG_LOGD(__func__);
  Init();
}

void CameraPrivate::Init() {
  channels_ = std::make_shared<Channels>();
  motions_ = std::make_shared<Motions>();
  streams_ = std::make_shared<Streams>(device_);

  if (channels_->IsAvaliable()) {
    ReadDeviceFlash();
  }
}

CameraPrivate::~CameraPrivate() {
  DBG_LOGD(__func__);
  Close();
  device_ = nullptr;
}

void CameraPrivate::GetDeviceInfos(std::vector<DeviceInfo>* dev_infos) const {
  return device_->GetDeviceInfos(dev_infos);
}

void CameraPrivate::GetStreamInfos(const std::int32_t& dev_index,
    std::vector<StreamInfo>* color_infos,
    std::vector<StreamInfo>* depth_infos) const {
  return device_->GetStreamInfos(dev_index, color_infos, depth_infos);
}

ErrorCode CameraPrivate::Open(const OpenParams& params) {
  if (IsOpened()) {
    return ErrorCode::SUCCESS;
  }

  bool ok = device_->Open(params);
  if (!ok) {
    return ErrorCode::ERROR_FAILURE;
  }

  if (ok) {
    StartDataTracking();
    streams_->OnCameraOpen();
    return ErrorCode::SUCCESS;
  } else {
    return ErrorCode::ERROR_CAMERA_OPEN_FAILED;
  }
}

bool CameraPrivate::IsOpened() const {
  return device_->IsOpened();
}

void CameraPrivate::CheckOpened() const {
  device_->CheckOpened();
}

std::shared_ptr<device::Descriptors> CameraPrivate::GetDescriptors() const {
  return descriptors_;
}

std::string CameraPrivate::GetDescriptor(const Descriptor &desc) const {
  if (!descriptors_) {
    LOGE("%s %d:: Device information not found", __FILE__, __LINE__);
    return "";
  }
  switch (desc) {
    case Descriptor::DEVICE_NAME:
      return descriptors_->name;
    case Descriptor::SERIAL_NUMBER:
      return descriptors_->serial_number;
    case Descriptor::FIRMWARE_VERSION:
      return descriptors_->firmware_version.to_string();
    case Descriptor::HARDWARE_VERSION:
      return descriptors_->hardware_version.to_string();
    case Descriptor::SPEC_VERSION:
      return descriptors_->spec_version.to_string();
    case Descriptor::LENS_TYPE:
      return descriptors_->lens_type.to_string();
    case Descriptor::IMU_TYPE:
      return descriptors_->imu_type.to_string();
    case Descriptor::NOMINAL_BASELINE:
      return std::to_string(descriptors_->nominal_baseline);
    default:
      LOGE("%s %d:: Unknown device info", __FILE__, __LINE__);
      return "";
  }
}

StreamIntrinsics CameraPrivate::GetStreamIntrinsics(
    const StreamMode& stream_mode) {
  if (!stream_intrinsics_) {
    stream_intrinsics_ = std::make_shared<StreamIntrinsics>();
  }
  auto streamData = GetCameraCalibration(stream_mode);
  stream_intrinsics_->left.width = streamData.InImgWidth/2;
  stream_intrinsics_->left.height = streamData.InImgHeight;
  stream_intrinsics_->left.fx = streamData.CamMat1[0];
  stream_intrinsics_->left.fy = streamData.CamMat1[4];
  stream_intrinsics_->left.cx = streamData.CamMat1[2];
  stream_intrinsics_->left.cy = streamData.CamMat1[5];
  for (int i = 0; i < 5; i++) {
    stream_intrinsics_->left.coeffs[i] = streamData.CamDist1[i];
  }

  stream_intrinsics_->right.width = streamData.InImgWidth/2;
  stream_intrinsics_->right.height = streamData.InImgHeight;
  stream_intrinsics_->right.fx = streamData.CamMat2[0];
  stream_intrinsics_->right.fy = streamData.CamMat2[4];
  stream_intrinsics_->right.cx = streamData.CamMat2[2];
  stream_intrinsics_->right.cy = streamData.CamMat2[5];
  for (int i = 0; i < 5; i++) {
    stream_intrinsics_->right.coeffs[i] = streamData.CamDist2[i];
  }
  return *stream_intrinsics_;
}

StreamExtrinsics CameraPrivate::GetStreamExtrinsics(
  const StreamMode& stream_mode) {
  if (!stream_extrinsics_) {
    stream_extrinsics_ = std::make_shared<StreamExtrinsics>();
  }
  auto streamData = GetCameraCalibration(stream_mode);
  for (int i = 0; i < 9; i++) {
    stream_extrinsics_->rotation[i/3][i%3] = streamData.RotaMat[i];
  }
  for (int j = 0; j < 3; j++) {
    stream_extrinsics_->translation[j] = streamData.TranMat[j];
  }
  return *stream_extrinsics_;
}

bool CameraPrivate::WriteCameraCalibrationBinFile(const std::string& filename) {
  return device_->SetCameraCalibrationBinFile(filename);
}

MotionIntrinsics CameraPrivate::GetMotionIntrinsics() const {
  if (motion_intrinsics_) {
    return *motion_intrinsics_;
  } else {
    LOGE("Error: Motion intrinsics not found");
    return {};
  }
}

MotionExtrinsics CameraPrivate::GetMotionExtrinsics() const {
  if (motion_extrinsics_) {
    return *motion_extrinsics_;
  } else {
    LOGE("Error: Motion extrinsics not found");
    return {};
  }
}

bool CameraPrivate::WriteDeviceFlash(
    device::Descriptors *desc,
    device::ImuParams *imu_params,
    Version *spec_version) {
  if (!channels_->IsAvaliable()) {
    LOGW("Data channel is unavaliable, could not write device datas.");
    return false;
  }
  return channels_->SetFiles(desc, imu_params, spec_version);
}

void CameraPrivate::EnableProcessMode(const ProcessMode& mode) {
  EnableProcessMode(static_cast<std::int32_t>(mode));
}

void CameraPrivate::EnableProcessMode(const std::int32_t& mode) {
  motions_->EnableProcessMode(mode);
}

void CameraPrivate::EnableImageInfo(bool sync) {
  streams_->EnableImageInfo(sync);
  StartDataTracking();
}

void CameraPrivate::EnableStreamData(const ImageType& type) {
  streams_->EnableStreamData(type);
}

bool CameraPrivate::IsStreamDataEnabled(const ImageType& type) {
  return streams_->IsStreamDataEnabled(type);
}

bool CameraPrivate::HasStreamDataEnabled() {
  return streams_->HasStreamDataEnabled();
}

StreamData CameraPrivate::GetStreamData(const ImageType& type) {
  return streams_->GetStreamData(type);
}

std::vector<StreamData> CameraPrivate::GetStreamDatas(const ImageType& type) {
  return streams_->GetStreamDatas(type);
}

void CameraPrivate::EnableMotionDatas(std::size_t max_size) {
  motions_->EnableMotionDatas(std::move(max_size));
  StartDataTracking();
}

std::vector<MotionData> CameraPrivate::GetMotionDatas() {
  return std::move(motions_->GetMotionDatas());
}

void CameraPrivate::SetImgInfoCallback(img_info_callback_t callback) {
  streams_->SetImgInfoCallback(callback);
}

void CameraPrivate::SetStreamCallback(const ImageType& type,
    stream_callback_t callback) {
  streams_->SetStreamCallback(type, callback);
}

void CameraPrivate::SetMotionCallback(motion_callback_t callback) {
  motions_->SetMotionCallback(callback);
}

void CameraPrivate::Close() {
  if (!IsOpened()) return;
  StopDataTracking();
  streams_->OnCameraClose();
  device_->Close();
}

CameraCalibration CameraPrivate::GetCameraCalibration(
    const StreamMode& stream_mode) {
  return device_->GetCameraCalibration(stream_mode);
}

void CameraPrivate::GetCameraCalibrationFile(
    const StreamMode& stream_mode, const std::string& filename) {
  return device_->GetCameraCalibrationFile(stream_mode, filename);
}

void CameraPrivate::ReadDeviceFlash() {
  if (!channels_->IsAvaliable()) {
    LOGW("Data channel is unavaliable, could not read device datas.");
    return;
  }
  descriptors_ = std::make_shared<device::Descriptors>();

  Channels::imu_params_t imu_params;
  if (!channels_->GetFiles(descriptors_.get(), &imu_params)) {
    LOGE("%s %d:: Read device descriptors failed. Please upgrade"
         "your firmware to the latest version.", __FILE__, __LINE__);
    return;
  }

  LOGI("\nDevice descriptors:");
  LOGI("  name: %s", descriptors_->name.c_str());
  LOGI("  serial_number: %s", descriptors_->serial_number.c_str());
  LOGI("  firmware_version: %s",
      descriptors_->firmware_version.to_string().c_str());
  LOGI("  hardware_version: %s",
      descriptors_->hardware_version.to_string().c_str());
  LOGI("  spec_version: %s", descriptors_->spec_version.to_string().c_str());
  LOGI("  lens_type: %s", descriptors_->lens_type.to_string().c_str());
  LOGI("  imu_type: %s", descriptors_->imu_type.to_string().c_str());
  LOGI("  nominal_baseline: %u", descriptors_->nominal_baseline);

  if (imu_params.ok) {
    SetMotionIntrinsics({imu_params.in_accel, imu_params.in_gyro});
    SetMotionExtrinsics(imu_params.ex_left_to_imu);
    // std::cout << GetMotionIntrinsics() << std::endl;
    // std::cout << GetMotionExtrinsics() << std::endl;
  } else {
    LOGE("%s %d:: Motion intrinsics & extrinsics not exist",
        __FILE__, __LINE__);
  }
}

void CameraPrivate::SetMotionIntrinsics(const MotionIntrinsics &in) {
  if (!motion_intrinsics_) {
    motion_intrinsics_ = std::make_shared<MotionIntrinsics>();
  }
  *motion_intrinsics_ = in;
  motions_->SetMotionIntrinsics(motion_intrinsics_);
}

void CameraPrivate::SetMotionExtrinsics(const MotionExtrinsics &ex) {
  if (!motion_extrinsics_) {
    motion_extrinsics_ = std::make_shared<MotionExtrinsics>();
  }
  *motion_extrinsics_ = ex;
}

bool CameraPrivate::StartDataTracking() {
  // if (!IsOpened()) return false;  // ensure start after opened
  if (!motions_->IsMotionDatasEnabled() && !streams_->IsImageInfoEnabled()) {
    // Not tracking when data & info both disabled.
    return false;
  }

  if (motions_->IsMotionDatasEnabled()) {
    channels_->SetImuDataCallback(std::bind(&Motions::OnImuDataCallback,
        motions_, std::placeholders::_1));
  }

  if (streams_->IsImageInfoEnabled()) {
    channels_->SetImgInfoCallback(std::bind(&Streams::OnImageInfoCallback,
          streams_, std::placeholders::_1));
  }

  if (channels_->IsHidTracking()) return true;

  if (!channels_->IsHidAvaliable()) {
    LOGW("Data channel is unavaliable, could not track device datas.");
    return false;
  }

  return channels_->StartHidTracking();
}

void CameraPrivate::StopDataTracking() {
  if (channels_->IsHidTracking()) {
    channels_->StopHidTracking();
  }
}
