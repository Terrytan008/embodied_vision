// 硬件抽象层 — 通用类型定义
// camera_types.hpp

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>

namespace stereo_vision::hardware {

// ============================================================
// 通用像素格式
// ============================================================
enum class PixelFormat : uint32_t {
    RAW12 = 0,
    RAW10 = 1,
    RAW8 = 2,
    RGB8 = 3,
    YUV422 = 4,
    INVALID = 0xFFFFFFFF,
};

// ============================================================
// 帧缓冲区
// ============================================================
struct FrameBuffer {
    void* data = nullptr;           // 原始数据指针
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::INVALID;
    uint64_t timestamp_ns = 0;      // 硬件时间戳
    uint32_t frame_id = 0;
    uint32_t stride = 0;           // 行字节数
    bool is_valid = false;

    FrameBuffer() = default;

    FrameBuffer(void* d, uint32_t w, uint32_t h, PixelFormat f,
                uint64_t ts, uint32_t id = 0)
        : data(d), width(w), height(h), format(f),
          timestamp_ns(ts), frame_id(id), is_valid(true) {}

    bool empty() const { return !is_valid || data == nullptr; }

    // 内存拷贝（安全）
    void copyTo(void* dst, size_t size) const {
        if (!empty() && dst) {
            std::memcpy(dst, data, std::min(size, static_cast<size_t>(stride * height)));
        }
    }
};

// ============================================================
// IMU原始数据
// ============================================================
struct IMURawData {
    int16_t gyro[3];       // 陀螺仪原始值
    int16_t accel[3];     // 加速度计原始值
    int16_t temp;          // 温度
    uint64_t timestamp_ns;
    bool valid = false;
};

// ============================================================
// 设备状态
// ============================================================
enum class DeviceState {
    Closed = 0,
    Opening = 1,
    Streaming = 2,
    Error = 3,
    Recovering = 4,
};

enum class ErrorCode : uint32_t {
    None = 0,
    DeviceNotFound = 0x1001,
    StreamStartFailed = 0x1002,
    I2CTimeout = 0x1003,
    CSIError = 0x1004,
    IMUCommFailed = 0x1005,
    Overheat = 0x1006,
    BufferOverflow = 0x1007,
};

// ============================================================
// 传感器信息
// ============================================================
struct SensorInfo {
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    uint16_t revision = 0;
    uint32_t serial = 0;
    char name[64] = {0};

    bool is_sony_imx678() const {
        return vendor_id == 0x0001 && product_id == 0x678;
    }
};

// ============================================================
// CSI-2 配置
// ============================================================
struct CSI2Config {
    uint8_t port = 0;           // CSI端口 (0-3)
    uint8_t lanes = 4;         // 通道数 (1/2/4)
    uint64_t freq_hz = 0;      // 时钟频率
    PixelFormat pixel_format = PixelFormat::RAW12;
    uint32_t width = 3840;
    uint32_t height = 2160;
};

// ============================================================
// IMU配置
// ============================================================
struct IMUConfig {
    uint8_t bus_id = 1;          // I2C总线
    uint8_t addr = 0x68;        // I2C地址 (BMI088=0x68)
    uint32_t gyro_range = 2000;  // 陀螺仪量程 (dps)
    uint32_t accel_range = 16;   // 加速度计量程 (g)
    uint32_t odr = 200;         // 输出数据率 (Hz)
    bool use_spi = false;        // SPI vs I2C
};

// ============================================================
// 设备配置
// ============================================================
struct DeviceConfig {
    CSI2Config csi2;
    IMUConfig imu;

    // I2C总线路径
    const char* i2c_bus = "/dev/i2c-1";

    // FPD-Link 解串器地址
    uint8_t deserializer_addr = 0x40;

    // V4L2 设备路径（模拟器模式下可为空）
    const char* left_device = "/dev/video0";
    const char* right_device = "/dev/video2";

    // 模拟器（开发用，不连接真实硬件）
    bool simulator_mode = false;
};

}  // namespace stereo_vision::hardware
