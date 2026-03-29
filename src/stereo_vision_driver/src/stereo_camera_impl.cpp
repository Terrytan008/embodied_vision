// Embodied Vision — 双目视觉模组核心实现
// stereo_camera_impl.cpp

#include "stereo_vision/stereo_camera.hpp"
#include <cstring>
#include <chrono>
#include <stdexcept>

namespace stereo_vision {

// ============================================================
// 实现类（Pimpl idiom）
// ============================================================
class StereoCamera::Impl {
public:
    Impl(const CameraConfig& config, void* node_base)
        : config_(config), node_base_(node_base) {}

    CameraConfig config_;
    void* node_base_ = nullptr;

    ErrorInfo last_error_{};
    DeviceStatus status_{};
    CalibrationData calib_{};

    // 模拟上一帧深度（用于速度校验）
    cv::Mat prev_depth_;
    uint64_t prev_timestamp_ns_ = 0;

    // 模拟帧计数器
    uint32_t frame_count_ = 0;

    bool validateAndUpdate(const cv::Mat& depth, uint64_t ts_ns) {
        if (prev_depth_.empty() || prev_depth_.size() != depth.size()) {
            prev_depth_ = depth.clone();
            prev_timestamp_ns_ = ts_ns;
            return true;
        }

        // 物理可行性检查
        float delta_t = (ts_ns - prev_timestamp_ns_) / 1e9f;
        if (delta_t <= 0.0f) return false;

        for (int y = 0; y < depth.rows; y++) {
            for (int x = 0; x < depth.cols; x++) {
                float d = depth.at<float>(y, x);
                if (d <= 0.0f) continue;

                float d_prev = prev_depth_.at<float>(y, x);
                if (d_prev <= 0.0f) continue;

                float delta_d = std::abs(d - d_prev);
                float velocity = delta_d / delta_t;

                if (velocity > 50.0f) {  // 50m/s 物理极限
                    return false;
                }
            }
        }

        prev_depth_ = depth.clone();
        prev_timestamp_ns_ = ts_ns;
        return true;
    }
};

// ============================================================
// 工厂方法（桩实现，待对接真实硬件）
// ============================================================
std::unique_ptr<StereoCamera>
StereoCamera::create(const CameraConfig& config, void* node_base) {
    // TODO: 这里将替换为真实硬件对接
    // 临时返回模拟实现
    struct MockCamera : public StereoCamera {
        Impl* impl_;

        explicit MockCamera(Impl* p) : impl_(p) {}
        ~MockCamera() override { delete impl_; }

        std::unique_ptr<FrameData> grabFrame(uint32_t timeout_ms) override {
            auto frame = std::make_unique<FrameData>();

            // 模拟640x480深度图
            frame->depth_m = cv::Mat(480, 640, CV_32FC1, cv::Scalar(1.5f));
            frame->confidence = cv::Mat(480, 640, CV_32FC1, cv::Scalar(0.85f));
            frame->disparity = cv::Mat(480, 640, CV_32FC1, cv::Scalar(128.0f));

            // 模拟校正后图像
            frame->left_rect = cv::Mat(480, 640, CV_8UC3, cv::Scalar(128));
            frame->right_rect = cv::Mat(480, 640, CV_8UC3, cv::Scalar(128));

            // 模拟元数据
            frame->metadata.hw_timestamp_ns =
                std::chrono::steady_clock::now().time_since_epoch().count();
            frame->metadata.system_timestamp_ns = frame->metadata.hw_timestamp_ns;
            frame->metadata.temperature_c = 42.0f;
            frame->metadata.is_high_motion = false;
            frame->metadata.motion_score = 0.1f;
            frame->metadata.hdr_exposure_ratio = 2;
            frame->metadata.left_valid_ratio = 95;
            frame->metadata.right_valid_ratio = 94;
            frame->metadata.overall_confidence = 85;
            frame->metadata.is_calibrated = true;
            frame->metadata.calib_quality = 0.92f;

            // IMU数据
            IMUData imu{};
            imu.gyro[0] = 0.001f; imu.gyro[1] = 0.002f; imu.gyro[2] = 0.001f;
            imu.accel[0] = 0.01f; imu.accel[1] = 9.81f; imu.accel[2] = 0.02f;
            imu.timestamp_ns = frame->metadata.hw_timestamp_ns;
            frame->imu = imu;

            // 物理可行性校验
            if (!impl_->validateAndUpdate(frame->depth_m, frame->metadata.hw_timestamp_ns)) {
                impl_->last_error_ = {
                    StereoError::SYNC_LOST,
                    "Depth velocity exceeds physical limit",
                    frame->metadata.hw_timestamp_ns,
                    true,  // recoverable
                    false  // not dangerous
                };
            }

            impl_->frame_count_++;
            return frame;
        }

        DeviceStatus getStatus() const override {
            DeviceStatus s;
            s.sensor_left_ok = true;
            s.sensor_right_ok = true;
            s.imu_ok = true;
            s.temp_sensor_ok = true;
            s.dropped_frames = impl_->frame_count_ > 100 ? 2 : 0;
            s.total_frames = impl_->frame_count_;
            s.frame_rate = 30.0f;
            s.temperature_c = 42.0f;
            s.last_hw_timestamp_ns =
                std::chrono::steady_clock::now().time_since_epoch().count();
            s.error_count = 0;
            s.level = DeviceStatus::Level::OK;
            return s;
        }

        CalibrationData getCalibration() const override {
            CalibrationData c;
            c.left_k = {1194.0f, 0.0f, 960.0f, 0.0f, 1194.0f, 540.0f, 0.0f, 0.0f, 1.0f};
            c.right_k = c.left_k;
            c.baseline_mm = 80.0f;
            c.recalib_confidence = 0.95f;
            c.recalib_timestamp_ns =
                std::chrono::steady_clock::now().time_since_epoch().count();
            return c;
        }

        void triggerRecalibration() override {
            // TODO: Thor GPU加速在线标定
        }

        ErrorInfo getLastError() const override {
            return impl_->last_error_;
        }

        void clearError() override {
            impl_->last_error_ = {};
        }

        std::string getVersion() const override {
            return "EmbodiedVision v0.1.0-alpha (mock implementation)";
        }
    };

    auto impl = new StereoCamera::Impl(config, node_base);
    return std::unique_ptr<StereoCamera>(new MockCamera(impl));
}

// ============================================================
// 工具函数实现
// ============================================================
cv::Mat confidenceMask(const cv::Mat& confidence, ConfidenceLevel min_level) {
    float threshold = 0.0f;
    switch (min_level) {
        case ConfidenceLevel::VERY_HIGH: threshold = 0.85f; break;
        case ConfidenceLevel::HIGH:      threshold = 0.65f; break;
        case ConfidenceLevel::MEDIUM:   threshold = 0.40f; break;
        case ConfidenceLevel::LOW:       threshold = 0.01f; break;
        default:                         threshold = 0.65f; break;
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
    // 简化实现：检查是否有任何异常大的深度跳变
    if (depth_m.empty()) return false;

    float prev_val = 0.0f;
    for (int y = 0; y < depth_m.rows; y++) {
        for (int x = 0; x < depth_m.cols; x++) {
            float d = depth_m.at<float>(y, x);
            if (d <= 0.0f) continue;
            if (prev_val > 0.0f) {
                float delta = std::abs(d - prev_val);
                if (delta > max_velocity_mps * 0.033f) { // ~33ms帧间隔
                    return false;
                }
            }
            prev_val = d;
        }
    }
    return true;
}

}  // namespace stereo_vision
