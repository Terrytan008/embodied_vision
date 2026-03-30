// Embodied Vision — 双目视觉模组核心API
// stereo_camera.hpp

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <array>
#include <cstdint>

#include <opencv2/opencv2.hpp>

namespace stereo_vision {

// ============================================================
// 设备配置
// ============================================================
struct CameraConfig {
    // --- 曝光控制 ---
    enum class ExposureMode { Auto, Manual, Fixed };
    ExposureMode exposure_mode = ExposureMode::Auto;
    uint32_t exposure_time_us = 33333;    // 30fps
    float analog_gain_db = 0.0f;

    // --- HDR模式 ---
    enum class HdrMode { Off, X2, X4 };
    HdrMode hdr_mode = HdrMode::X2;

    // --- 深度范围 ---
    float depth_min_m = 0.1f;
    float depth_max_m = 10.0f;

    // --- 置信度 ---
    float confidence_threshold = 0.65f;    // 默认阈值

    // --- 基线（默认使用标定值） ---
    std::optional<float> baseline_mm;

    // --- ONNX 置信度模型路径（空=不使用 NN 推理）---
    std::string onnx_model_path;

    // --- ROS2 发布配置 ---
    bool publish_left = true;
    bool publish_right = true;
    bool publish_depth = true;
    bool publish_confidence = true;
    int publish_hz = 10;
};

// ============================================================
// IMU数据结构
// ============================================================
struct IMUData {
    float gyro[3];       // 角速度 rad/s (x, y, z)
    float accel[3];     // 加速度 m/s² (x, y, z)
    uint64_t timestamp_ns;
};

// ============================================================
// 帧元数据
// ============================================================
struct FrameMetadata {
    uint64_t hw_timestamp_ns;      // 硬件快门时间戳
    uint64_t system_timestamp_ns;  // 系统接收时间戳
    float temperature_c;           // 模组温度

    // 运动状态（自动判定）
    bool is_high_motion = false;
    float motion_score = 0.0f;    // 0=静止, 1=高速

    // HDR状态
    uint8_t hdr_exposure_ratio = 1;  // 1=HDR关闭

    // 有效像素比例
    uint8_t left_valid_ratio = 0;
    uint8_t right_valid_ratio = 0;
    uint8_t overall_confidence = 0;  // 综合评分 0-100

    // 标定状态
    bool is_calibrated = false;
    float calib_quality = 0.0f;
};

// ============================================================
// 自包含帧数据（核心输出结构）
// ============================================================
struct FrameData {
    cv::Mat left_rect;     // 左目校正后图像
    cv::Mat right_rect;    // 右目校正后图像
    cv::Mat depth_m;       // 深度图 (米), CV_32FC1, 无效值=0
    cv::Mat confidence;    // 置信度图 (0.0~1.0), CV_32FC1
    cv::Mat disparity;     // 视差图 (像素), CV_32FC1

    // IMU同步数据（可选）
    std::optional<IMUData> imu;

    // 元数据
    FrameMetadata metadata;
};

// ============================================================
// 设备状态
// ============================================================
struct DeviceStatus {
    bool sensor_left_ok = false;
    bool sensor_right_ok = false;
    bool imu_ok = false;
    bool temp_sensor_ok = false;

    uint32_t dropped_frames = 0;
    uint32_t total_frames = 0;
    float frame_rate = 0.0f;
    float temperature_c = 0.0f;
    uint64_t last_hw_timestamp_ns = 0;
    uint32_t error_count = 0;
    std::string last_error_msg;

    enum class Level { OK = 0, WARNING = 1, ERROR = 2 };
    Level level = Level::OK;
};

// ============================================================
// 标定数据
// ============================================================
struct CalibrationData {
    std::array<float, 9> left_k;       // 左目内参矩阵
    std::array<float, 9> right_k;     // 右目内参矩阵
    std::array<float, 12> T_lr;       // 左右目外参 (RT 3x4)
    std::array<float, 5> left_d;       // 左目畸变系数
    std::array<float, 5> right_d;      // 右目畸变系数
    float baseline_mm = 80.0f;         // 基线距离
    float recalib_confidence = 0.0f;  // 在线标定置信度
    uint64_t recalib_timestamp_ns = 0; // 最后标定时间戳
};

// ============================================================
// 错误码
// ============================================================
enum class StereoError : uint32_t {
    OK = 0,

    // 设备级（需人工介入）
    DEVICE_NOT_FOUND = 0x1001,
    SENSOR_HARDWARE_FAULT = 0x1002,
    I2C_COMM_TIMEOUT = 0x1003,
    TEMP_OVERHEAT = 0x1004,
    FIRMWARE_CRASH = 0x1005,

    // 数据级（可自动恢复）
    SYNC_LOST = 0x2001,
    CALIB_INVALID = 0x2002,
    FRAME_DROPPED = 0x2003,
    LOW_CONFIDENCE_RATIO = 0x2004,

    // API使用错误
    INVALID_PARAMETER = 0x3001,
    ALREADY_STARTED = 0x3002,
    NOT_STARTED = 0x3003,
};

struct ErrorInfo {
    StereoError code = StereoError::OK;
    std::string message;
    uint64_t timestamp_ns = 0;
    bool recoverable = false;   // 是否可自动恢复
    bool dangerous = false;     // 是否可能导致危险输出
};

// ============================================================
// 置信度等级（下游决策辅助）
// ============================================================
enum class ConfidenceLevel {
    INVALID = 0,       // 完全无效（传感器失效）
    LOW = 1,           // 仅适合VIO融合
    MEDIUM = 2,        // 仅适合语义感知
    HIGH = 3,          // 适合导航避障
    VERY_HIGH = 4      // 适合精密操作
};

inline ConfidenceLevel classifyConfidence(float conf) {
    if (conf >= 0.85f) return ConfidenceLevel::VERY_HIGH;
    if (conf >= 0.65f) return ConfidenceLevel::HIGH;
    if (conf >= 0.40f) return ConfidenceLevel::MEDIUM;
    if (conf > 0.0f)   return ConfidenceLevel::LOW;
    return ConfidenceLevel::INVALID;
}

// ============================================================
// 双目相机主类
// ============================================================
class StereoCamera {
public:
    /**
     * @brief 创建设备实例
     * @param config 设备配置（默认参数即可正常工作）
     * @param node_base ROS2 Node指针（用于发布话题，可为nullptr）
     * @return 设备实例，失败返回nullptr
     */
    static std::unique_ptr<StereoCamera>
    create(const CameraConfig& config = CameraConfig(),
           void* node_base = nullptr);

    /**
     * @brief 获取一帧数据（主要接口）
     * @param timeout_ms 超时时间（毫秒）
     * @return 帧数据，超时返回nullptr
     *
     * @note 此调用会阻塞直到新帧到达或超时
     *       返回的FrameData不得跨线程使用
     */
    virtual std::unique_ptr<FrameData>
    grabFrame(uint32_t timeout_ms = 100) = 0;

    /**
     * @brief 获取设备状态
     */
    virtual DeviceStatus getStatus() const = 0;

    /**
     * @brief 获取当前标定参数
     */
    virtual CalibrationData getCalibration() const = 0;

    /**
     * @brief 触发在线标定重收敛
     * @note Thor平台<1秒完成，建议在碰撞或温度变化>10°C后调用
     */
    virtual void triggerRecalibration() = 0;

    /**
     * @brief 获取最近一次错误
     */
    virtual ErrorInfo getLastError() const = 0;

    /**
     * @brief 清除错误状态，尝试恢复
     */
    virtual void clearError() = 0;

    /**
     * @brief 获取版本信息
     */
    virtual std::string getVersion() const = 0;

    virtual ~StereoCamera() = default;

protected:
    StereoCamera() = default;
    StereoCamera(const StereoCamera&) = delete;
    StereoCamera& operator=(const StereoCamera&) = delete;
};

// ============================================================
// 便捷工具函数
// ============================================================

/**
 * @brief 从置信度图生成掩码
 * @param confidence 置信度图
 * @param min_level 最低接受的置信度等级
 * @return 二值掩码（非零=有效）
 */
cv::Mat confidenceMask(const cv::Mat& confidence,
                       ConfidenceLevel min_level = ConfidenceLevel::HIGH);

/**
 * @brief 将置信度图转换为伪彩色可视化
 */
cv::Mat confidenceColorize(const cv::Mat& confidence);

/**
 * @brief 深度图安全性检查
 * @return true=安全可用，false=存在异常跳变
 */
bool validateDepthSafety(const cv::Mat& depth_m,
                         float max_velocity_mps = 50.0f);

}  // namespace stereo_vision
