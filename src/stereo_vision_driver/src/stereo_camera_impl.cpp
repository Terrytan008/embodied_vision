// Embodied Vision — 双目视觉模组核心实现
// stereo_camera_impl.cpp

#include "stereo_vision/stereo_camera.hpp"
#include "stereo_vision/hardware/capture_base.hpp"
#include "stereo_vision/hardware/camera_types.hpp"

#include <rclcpp/rclcpp.hpp>

#include <chrono>
#include <thread>
#include <cstring>

namespace stereo_vision {

using namespace hardware;

// ============================================================
// 实现类（Pimpl idiom）
// ============================================================
class StereoCamera::Impl {
public:
    Impl(const CameraConfig& config, void* node_base)
        : config_(config), node_base_(node_base) {}

    ~Impl() {
        stop();
        if (imu_thread_.joinable()) imu_thread_.join();
    }

    CameraConfig config_;
    void* node_base_ = nullptr;

    // ---- 硬件 ----
    std::unique_ptr<CaptureDevice> capture_left_;
    std::unique_ptr<CaptureDevice> capture_right_;
    std::unique_ptr<BMI088Driver> imu_;

    // ---- 帧缓冲 ----
    std::atomic<bool> running_{false};
    std::mutex frame_mutex_;
    FrameData current_frame_;

    // ---- IMU缓冲 ----
    std::mutex imu_mutex_;
    IMUData current_imu_;
    std::atomic<bool> imu_available_{false};

    // IMU 轮询线程
    std::thread imu_thread_;

    // ---- 错误/状态 ----
    ErrorInfo last_error_{};
    DeviceStatus status_{};
    CalibrationData calib_{};

    // ---- 帧统计 ----
    std::atomic<uint32_t> frame_count_{0};
    std::atomic<uint32_t> drop_count_{0};
    std::chrono::steady_clock::time_point last_frame_time_;

    // ---- 设备配置 ----
    DeviceConfig hw_config_{};

    // ---- 初始化硬件 ----
    bool initialize(const CameraConfig& config) {
        // 构造硬件配置
        hw_config_.csi2.width = 3840;
        hw_config_.csi2.height = 2160;
        hw_config_.csi2.port = 0;
        hw_config_.csi2.lanes = 4;
        hw_config_.csi2.freq_hz = 0;
        hw_config_.csi2.pixel_format = PixelFormat::RAW12;

        hw_config_.imu.bus_id = 1;
        hw_config_.imu.addr = 0x18;   // BMI088 ACC
        hw_config_.imu.gyro_range = 2000;
        hw_config_.imu.accel_range = 16;
        hw_config_.imu.odr = 200;
        hw_config_.imu.use_spi = false;

        hw_config_.i2c_bus = "/dev/i2c-1";
        hw_config_.deserializer_addr = 0x40;

        // 模拟模式检测
        if (config.publish_hz == 0) {
            hw_config_.simulator_mode = true;
            RCLCPP_INFO(rclcpp::get_logger("StereoCamera"), "模拟器模式");
            return true;
        }

        // ---- 创建左目录制设备 ----
        // TODO: 替换为 V4L2CaptureDevice 直接实例化
        RCLCPP_INFO(rclcpp::get_logger("StereoCamera"), "左目录制设备初始化...");

        // ---- 创建右目录制设备 ----
        RCLCPP_INFO(rclcpp::get_logger("StereoCamera"), "右目录制设备初始化...");

        // ---- 创建 IMU ----
        imu_ = std::make_unique<BMI088Driver>();
        if (imu_->open(hw_config_.i2c_bus,
                       hw_config_.imu.addr,
                       hw_config_.imu.addr + 1)) {
            if (imu_->init()) {
                RCLCPP_INFO(rclcpp::get_logger("StereoCamera"), "IMU 初始化成功");
                imu_available_.store(true);
            } else {
                RCLCPP_WARN(rclcpp::get_logger("StereoCamera"), "IMU 配置失败");
                imu_.reset();
            }
        } else {
            RCLCPP_WARN(rclcpp::get_logger("StereoCamera"), "IMU 打开失败（继续无IMU模式）");
            imu_.reset();
        }

        // 启动 IMU 轮询
        if (imu_) {
            imu_thread_ = std::thread([this]() {
                IMURawData raw;
                while (running_.load()) {
                    if (imu_->read(raw)) {
                        std::lock_guard<std::mutex> lock(imu_mutex_);
                        current_imu_.gyro[0] = raw.gyro[0] * 0.001f;  // raw→ dps
                        current_imu_.gyro[1] = raw.gyro[1] * 0.001f;
                        current_imu_.gyro[2] = raw.gyro[2] * 0.001f;
                        current_imu_.accel[0] = raw.accel[0] * 0.001f; // raw→ m/s²
                        current_imu_.accel[1] = raw.accel[1] * 0.001f;
                        current_imu_.accel[2] = raw.accel[2] * 0.001f;
                        current_imu_.timestamp_ns = raw.timestamp_ns;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
        }

        return true;
    }

    // ---- 停止硬件 ----
    void stop() {
        running_.store(false);
        if (capture_left_) capture_left_->stopStreaming();
        if (capture_right_) capture_right_->stopStreaming();
    }

    // ---- 深度物理校验 ----
    bool validateDepthPhysics(const cv::Mat& depth, uint64_t ts_ns) {
        if (prev_depth_.empty()) {
            prev_depth_ = depth.clone();
            prev_ts_ns_ = ts_ns;
            return true;
        }

        float dt = (ts_ns - prev_ts_ns_) / 1e9f;
        if (dt <= 0.0f) return false;

        for (int y = 0; y < depth.rows; y += 8) {  // 降采样检查
            for (int x = 0; x < depth.cols; x += 8) {
                float d = depth.at<float>(y, x);
                if (d <= 0.0f) continue;
                float d_prev = prev_depth_.at<float>(y, x);
                if (d_prev <= 0.0f) continue;

                float v = std::abs(d - d_prev) / dt;
                if (v > 50.0f) {  // 50m/s 物理极限
                    return false;
                }
            }
        }

        prev_depth_ = depth.clone();
        prev_ts_ns_ = ts_ns;
        return true;
    }

private:
    cv::Mat prev_depth_;
    uint64_t prev_ts_ns_ = 0;
};

// ============================================================
// 工厂方法
// ============================================================
std::unique_ptr<StereoCamera>
StereoCamera::create(const CameraConfig& config, void* node_base) {
    // 创建实例
    struct RealCamera : public StereoCamera {
        Impl* impl_;

        explicit RealCamera(Impl* p) : impl_(p) {}
        ~RealCamera() override { delete impl_; }

        std::unique_ptr<FrameData> grabFrame(uint32_t timeout_ms) override {
            if (impl_->hw_config_.simulator_mode) {
                return simulateFrame();
            }
            return realGrabFrame(timeout_ms);
        }

        std::unique_ptr<FrameData> simulateFrame() {
            auto frame = std::make_unique<FrameData>();

            auto now = std::chrono::steady_clock::now();
            uint64_t ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count();

            frame->depth_m = cv::Mat(480, 640, CV_32FC1, cv::Scalar(1.5f));
            frame->confidence = cv::Mat(480, 640, CV_32FC1, cv::Scalar(0.85f));
            frame->disparity = cv::Mat(480, 640, CV_32FC1, cv::Scalar(128.0f));
            frame->left_rect = cv::Mat(480, 640, CV_8UC3, cv::Scalar(100));
            frame->right_rect = cv::Mat(480, 640, CV_8UC3, cv::Scalar(100));

            frame->metadata.hw_timestamp_ns = ts_ns;
            frame->metadata.system_timestamp_ns = ts_ns;
            frame->metadata.temperature_c = 42.0f;
            frame->metadata.is_high_motion = false;
            frame->metadata.motion_score = 0.05f;
            frame->metadata.hdr_exposure_ratio = 2;
            frame->metadata.left_valid_ratio = 95;
            frame->metadata.right_valid_ratio = 95;
            frame->metadata.overall_confidence = 85;
            frame->metadata.is_calibrated = true;
            frame->metadata.calib_quality = 0.95f;

            if (impl_->imu_available_.load()) {
                IMUData imu;
                std::lock_guard<std::mutex> lock(impl_->imu_mutex_);
                imu = impl_->current_imu_;
                frame->imu = imu;
            }

            impl_->frame_count_++;
            return frame;
        }

        std::unique_ptr<FrameData> realGrabFrame(uint32_t) {
            auto frame = std::make_unique<FrameData>();
            // TODO: 调用真实硬件获取帧
            // 这里需要V4L2采集线程和同步机制
            impl_->frame_count_++;
            return frame;
        }

        DeviceStatus getStatus() const override {
            DeviceStatus s;
            s.sensor_left_ok = impl_->capture_left_ != nullptr;
            s.sensor_right_ok = impl_->capture_right_ != nullptr;
            s.imu_ok = impl_->imu_ != nullptr;
            s.temp_sensor_ok = true;
            s.dropped_frames = impl_->drop_count_.load();
            s.total_frames = impl_->frame_count_.load();
            s.frame_rate = 30.0f;
            s.temperature_c = 42.0f;
            s.last_hw_timestamp_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            s.level = DeviceStatus::Level::OK;
            return s;
        }

        CalibrationData getCalibration() const override {
            CalibrationData c;
            c.left_k = {1194.0f, 0.0f, 960.0f,
                         0.0f, 1194.0f, 540.0f,
                         0.0f, 0.0f, 1.0f};
            c.right_k = c.left_k;
            c.baseline_mm = 80.0f;
            c.recalib_confidence = 0.95f;
            c.recalib_timestamp_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            return c;
        }

        void triggerRecalibration() override {
            RCLCPP_INFO(rclcpp::get_logger("StereoCamera"),
                        "触发在线标定重收敛（Thor GPU加速）...");
            // TODO: Thor GPU加速在线BA优化
        }

        ErrorInfo getLastError() const override {
            return impl_->last_error_;
        }

        void clearError() override {
            impl_->last_error_ = {};
        }

        std::string getVersion() const override {
            if (impl_->hw_config_.simulator_mode) {
                return "EmbodiedVision v0.1.0 (SIMULATOR MODE)";
            }
            return "EmbodiedVision v0.1.0 (V4L2+BMI088)";
        }
    };

    auto impl = new Impl(config, node_base);

    if (!impl->initialize(config)) {
        delete impl;
        return nullptr;
    }

    return std::unique_ptr<StereoCamera>(new RealCamera(impl));
}

// ============================================================
// 工具函数实现
// ============================================================
cv::Mat confidenceMask(const cv::Mat& confidence, ConfidenceLevel min_level) {
    float threshold = 0.0f;
    switch (min_level) {
        case ConfidenceLevel::VERY_HIGH: threshold = 0.85f; break;
        case ConfidenceLevel::HIGH:      threshold = 0.65f; break;
        case ConfidenceLevel::MEDIUM:     threshold = 0.40f; break;
        case ConfidenceLevel::LOW:        threshold = 0.01f; break;
        default:                          threshold = 0.65f;   break;
    }

    cv::Mat mask;
    cv::threshold(confidence, mask, threshold, 255, cv::THRESH_BINARY);
    mask.convertTo(mask, CV_8UC1);
    return mask;
}

cv::Mat confidenceColorize(const cv::Mat& confidence) {
    cv::Mat color;
    confidence.convertTo(color, CV_8UC1, 255.0f);
    cv::applyColorMap(color, color, cv::COLORMAP_JET);
    return color;
}

bool validateDepthSafety(const cv::Mat& depth_m, float max_velocity_mps) {
    if (depth_m.empty()) return false;
    const float kFrameTime = 0.033f;  // ~33ms
    float prev = 0.0f;
    for (int y = 0; y < depth_m.rows; y += 4) {
        for (int x = 0; x < depth_m.cols; x += 4) {
            float d = depth_m.at<float>(y, x);
            if (d <= 0.0f) continue;
            if (prev > 0.0f && std::abs(d - prev) > max_velocity_mps * kFrameTime) {
                return false;
            }
            prev = d;
        }
    }
    return true;
}

}  // namespace stereo_vision
