// V4L2 CSI-2 捕获设备
// v4l2_capture.hpp

#pragma once

#include "capture_base.hpp"

namespace stereo_vision::hardware {

/**
 * @brief V4L2 Capture 设备实现
 *
 * 适用于标准 Linux (非 Jetson/DRIVE 平台)
 * 通过 V4L2 API 捕获 CSI-2 摄像头数据
 */
class V4L2CaptureDevice : public CaptureDevice {
public:
    V4L2CaptureDevice();
    ~V4L2CaptureDevice() override;

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
    bool initDevice(const DeviceConfig& config);
    bool startCaptureLoop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace stereo_vision::hardware
