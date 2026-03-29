// Embodied Vision — NVIDIA NvMedia CSI-2 原生捕获实现
// nv_nvmedia_capture.cpp
//
// 适用于：NVIDIA DRIVE AGX Thor / Jetson AGX Orin / Jetson NX
//
// 注意：完整实现需要 NVIDIA DriveOS SDK / JetPack
// 当前为存根实现（不含NvMedia SDK时可编译）

#include "stereo_vision/hardware/nv_nvmedia_capture.hpp"

#include <rclcpp/rclcpp.hpp>

#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstring>

namespace stereo_vision::hardware {

// ============================================================================
// 平台检测
// ============================================================================

namespace {
bool isNvidiaTegraPlatform() {
    std::ifstream f("/proc/device-tree/compatible");
    if (!f.is_open()) return false;
    char buf[512];
    f.read(buf, sizeof(buf) - 1);
    return std::strstr(buf, "nvidia,tegra") != nullptr;
}
}

// ============================================================================
// 公共接口实现
// ============================================================================

NvMediaCaptureDevice::NvMediaCaptureDevice()
    : state_(DeviceState::Closed),
      last_error_(ErrorCode::None) {
    RCLCPP_INFO(logger_, "NvMediaCaptureDevice 构造");
}

NvMediaCaptureDevice::~NvMediaCaptureDevice() {
    close();
}

bool NvMediaCaptureDevice::open(const DeviceConfig& config) {
    config_ = config;
    state_.store(DeviceState::Streaming);

    if (!isNvidiaTegraPlatform()) {
        RCLCPP_WARN(logger_, "非 NVIDIA 平台，NvMedia 不可用");
        RCLCPP_WARN(logger_, "实际需要 NVIDIA DriveOS/JetPack SDK");
        last_error_.store(ErrorCode::DeviceNotFound);
        state_.store(DeviceState::Error);
        return false;
    }

    sensor_info_.name = "NvMedia CSI-2 Camera (存根)";
    sensor_info_.width = config.csi2.width;
    sensor_info_.height = config.csi2.height;

    RCLCPP_INFO(logger_, "NvMedia 设备打开成功（存根模式）");
    return true;
}

bool NvMediaCaptureDevice::startStreaming(FrameCallback callback) {
    callback_ = callback;
    streaming_.store(true);

    capture_thread_ = std::thread([this]() {
        RCLCPP_INFO(logger_, "NvMedia 采集线程启动（存根）");

        // 存根：模拟帧生成（无真实硬件时用于测试）
        // 实际需要 NvMedia IJP API
        uint64_t frame_count = 0;
        while (streaming_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));  // 30fps

            if (!callback_) continue;

            // 构造模拟帧
            FrameBuffer fake_frame;
            fake_frame.width = config_.csi2.width;
            fake_frame.height = config_.csi2.height;
            fake_frame.timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            fake_frame.format = PixelFormat::RAW12;
            fake_frame.data = nullptr;  // 无真实数据
            fake_frame.length = 0;

            IMURawData empty_imu{};
            empty_imu.timestamp_ns = fake_frame.timestamp_ns;

            callback_(fake_frame, {}, empty_imu);
            frame_count++;
        }

        RCLCPP_INFO(logger_, "NvMedia 采集线程结束，%lu 帧", frame_count);
    });

    return true;
}

void NvMediaCaptureDevice::stopStreaming() {
    if (!streaming_.load()) return;
    streaming_.store(false);
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    RCLCPP_INFO(logger_, "NvMedia 流已停止");
}

void NvMediaCaptureDevice::close() {
    stopStreaming();
    state_.store(DeviceState::Closed);
}

DeviceState NvMediaCaptureDevice::getState() const {
    return state_.load(std::memory_order_acquire);
}

ErrorCode NvMediaCaptureDevice::getLastError() const {
    return last_error_.load(std::memory_order_acquire);
}

SensorInfo NvMediaCaptureDevice::getSensorInfo() const {
    return sensor_info_;
}

std::string NvMediaCaptureDevice::getVersion() const {
    return version_ + " (存根 - 需要 DriveOS/JetPack SDK)";
}

bool NvMediaCaptureDevice::setExposure(uint32_t us) {
    (void)us;
    RCLCPP_INFO(logger_, "NvMedia 曝光设置（存根）: %u us", us);
    return true;
}

bool NvMediaCaptureDevice::setAnalogGain(float db) {
    (void)db;
    RCLCPP_INFO(logger_, "NvMedia 增益设置（存根）: %.1f dB", db);
    return true;
}

bool NvMediaCaptureDevice::setHdrMode(int mode) {
    (void)mode;
    RCLCPP_INFO(logger_, "NvMedia HDR 模式设置（存根）: %d", mode);
    return true;
}

}  // namespace stereo_vision::hardware
