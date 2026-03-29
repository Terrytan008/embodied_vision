// NVIDIA NvMedia CSI-2 捕获设备
// nv_nvmedia_capture.hpp

#pragma once

#include "capture_base.hpp"
#include "camera_types.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

// 前向声明NvMedia类型（避免污染全局命名空间）
struct NvMediaDevice;
struct NvMediaIJPObj;
struct NvMediaBufPool;
struct NvMediaImage;

namespace stereo_vision::hardware {

/**
 * @brief NvMedia CSI-2 Capture 设备实现
 *
 * 适用于 NVIDIA DRIVE AGX Thor / Jetson AGX Orin / Jetson NX
 * 使用 NvMedia API 直接访问 CSI-2 端口，实现零拷贝 GPU 内存传递
 *
 * 注意：完整实现需要 NVIDIA DriveOS SDK / JetPack
 * 当前为存根，仅供编译通过
 */
class NvMediaCaptureDevice : public CaptureDevice {
public:
    NvMediaCaptureDevice();
    ~NvMediaCaptureDevice() override;

    bool open(const DeviceConfig& config) override;
    bool startStreaming(FrameCallback callback) override;
    void stopStreaming() override;
    void close() override;
    DeviceState getState() const override;
    ErrorCode getLastError() const override;
    SensorInfo getSensorInfo() const override;
    std::string getVersion() const override;

    bool setExposure(uint32_t us) override;
    bool setAnalogGain(float db) override;
    bool setHdrMode(int mode) override;

private:
    // ---- 内部成员（占位）----
    DeviceConfig config_;
    std::atomic<DeviceState> state_{DeviceState::Closed};
    std::atomic<ErrorCode> last_error_{ErrorCode::None};
    SensorInfo sensor_info_;
    std::string version_ = "NvMedia CSI-2 Capture (存根)";

    std::thread capture_thread_;
    std::atomic<bool> streaming_{false};
    CaptureDevice::FrameCallback callback_;
    rclcpp::Logger logger_ = rclcpp::get_logger("NvMediaCapture");
};

}  // namespace stereo_vision::hardware
