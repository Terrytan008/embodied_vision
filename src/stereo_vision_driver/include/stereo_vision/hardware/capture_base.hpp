// 硬件抽象层 — Capture 基类
// capture_base.hpp

#pragma once

#include "camera_types.hpp"
#include <functional>
#include <memory>
#include <string>

namespace stereo_vision::hardware {

/**
 * @brief Capture 设备抽象基类
 *
 * 不同的 CSI-2 捕获实现（V4L2 / NvMedia / 自定义）
 * 都必须实现这个接口。
 */
class CaptureDevice {
public:
    using FrameCallback = std::function<void(const FrameBuffer& left,
                                            const FrameBuffer& right,
                                            const IMURawData& imu)>;

    CaptureDevice() = default;
    virtual ~CaptureDevice() = default;

    // 禁止拷贝
    CaptureDevice(const CaptureDevice&) = delete;
    CaptureDevice& operator=(const CaptureDevice&) = delete;

    // ---- 通用接口 ----

    /**
     * @brief 打开设备
     * @param config 设备配置
     * @return true=成功
     */
    virtual bool open(const DeviceConfig& config) = 0;

    /**
     * @brief 开始流
     * @param callback 每帧回调（同步调用，不能阻塞）
     */
    virtual bool startStreaming(FrameCallback callback) = 0;

    /**
     * @brief 停止流
     */
    virtual void stopStreaming() = 0;

    /**
     * @brief 关闭设备
     */
    virtual void close() = 0;

    /**
     * @brief 获取设备状态
     */
    virtual DeviceState getState() const = 0;

    /**
     * @brief 获取错误信息
     */
    virtual ErrorCode getLastError() const = 0;

    /**
     * @brief 获取传感器信息
     */
    virtual SensorInfo getSensorInfo() const = 0;

    /**
     * @brief 获取设备版本
     */
    virtual std::string getVersion() const = 0;

    // ---- 平台特有接口（可选） ----

    /**
     * @brief 设置曝光（0=自动）
     */
    virtual bool setExposure(uint32_t us) {
        (void)us;
        return false;  // 默认不支持
    }

    /**
     * @brief 设置模拟增益 dB
     */
    virtual bool setAnalogGain(float db) {
        (void)db;
        return false;
    }

    /**
     * @brief 设置 HDR 模式
     */
    virtual bool setHdrMode(int mode) {
        (void)mode;
        return false;
    }

protected:
    DeviceState state_ = DeviceState::Closed;
    ErrorCode last_error_ = ErrorCode::None;
};

// ============================================================
// 工厂函数（平台选择）
// ============================================================

/**
 * @brief 创建平台相关的 Capture 设备
 *
 * 自动检测平台：
 *   - NVIDIA Jetson / DRIVE → NvCaptureDevice
 *   - 其他 Linux → V4L2CaptureDevice
 *
 * @param platform 强制指定平台（为空则自动检测）
 */
std::unique_ptr<CaptureDevice>
createCaptureDevice(const std::string& platform = "");

}  // namespace stereo_vision::hardware
