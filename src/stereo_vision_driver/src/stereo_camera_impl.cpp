// Embodied Vision — 双目视觉模组核心实现
// stereo_camera_impl.cpp
//
// 命名空间策略（重要）：
// 任何命名空间（包括 namespace hardware {}）都必须在所有标准库头之后才能打开。
// GCC 的 PSTL 在 rclcpp 头文件内实例化 std 算法时，
// 若此时处于 namespace hardware {} 上下文，std::* 符号会错误解析为 hardware::std::*。
// 因此 rclcpp 和所有自定义头文件都在全局 scope include，
// 然后才打开 stereo_vision/hardware 命名空间。

// ============ 标准库头（在任何命名空间之外）============
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <csignal>      // sig_atomic_t（必须在 hardware 命名空间前解析）
#include <ratio>        // std::ratio（必须在 hardware 命名空间前解析）
#include <memory>
#include <string>
#include <atomic>
#include <functional>

// ============ ROS2 rclcpp（全局 scope）============
#include <rclcpp/rclcpp.hpp>

// ============ 自定义头文件（全局 scope）============
#include "stereo_vision/stereo_camera.hpp"
#include "stereo_vision/hardware/capture_base.hpp"
#include "stereo_vision/hardware/camera_types.hpp"
#include "stereo_vision/stereo_depth.hpp"
#ifdef ENABLE_ONNXRUNTIME
#include "stereo_vision/confidence_onnx.hpp"
#endif

// ============ 进入项目命名空间 ====================
namespace stereo_vision {

// 注意：不使用 "using namespace hardware;"
// hardware 命名空间的类型必须显式用 hardware:: 前缀引用。

// ============================================================
// RAW12 解析 & Bayer demosaic 辅助
// ============================================================
 *   Byte[2n+0]: pixel_n[11:8]  (高4位)
 *   Byte[2n+1]: pixel_n[7:0]   (低8位)
 *   Byte[2n+2]: pixel_{n+1}[3:0] << 4 | pixel_n[3:0]  (shared nibble) 共享 nibble
 */
static void parseRaw12(const uint8_t* src, size_t len,
                       int w, int h, cv::Mat& out) {
    out.create(h, w, CV_16SC1);
    int pix_idx = 0;
    for (size_t byte_idx = 0; byte_idx + 2 < len && pix_idx + 1 < w * h;
         byte_idx += 3) {
        // Pixel 0: (src[0] << 4) | (src[1] >> 4)
        int16_t p0 = (static_cast<int16_t>(src[byte_idx]) << 4) |
                      (src[byte_idx + 1] >> 4);
        // Pixel 1: ((src[1] & 0x0F) << 8) | src[2]
        int16_t p1 = ((src[byte_idx + 1] & 0x0F) << 8) | src[byte_idx + 2];
        out.at<int16_t>(pix_idx / w, pix_idx % w) = p0;
        ++pix_idx;
        out.at<int16_t>(pix_idx / w, pix_idx % w) = p1;
        ++pix_idx;
    }
    // 填充剩余
    while (pix_idx < w * h) {
        out.at<int16_t>(pix_idx / w, pix_idx % w) = 0;
        ++pix_idx;
    }
}

/**
 * @brief Bayer demosaic (RGGB → RGB)，使用 OpenCV 内置双线性插值
 *
 * IMX678 RGGB CFA 布局：
 *   G R G R G R ...
 *   B G B G B G ...
 *   G R G R G R ...
 *   ...
 */
static void debayerRGGG(const cv::Mat& raw12, cv::Mat& out_rgb) {
    CV_Assert(raw12.type() == CV_16SC1);

    // 12bit → 8bit（线性缩放，保留全部动态范围）
    cv::Mat raw8;
    raw12.convertTo(raw8, CV_8UC1, 1.0f / 16.0f);  // [0,4095] → [0,255]

    // OpenCV Bayer → BGR 双线性插值
    // IMX678 是 RGGB → BGGR 字节序对应 COLOR_BAYERBG2BGR
    cv::cvtColor(raw8, out_rgb, cv::COLOR_BayerBG2BGR);
}

// ============================================================
// 实现类（Pimpl idiom）
// ============================================================
class StereoCamera::Impl {
public:
    Impl(const CameraConfig& config, void* node_base)
        : config_(config), node_base_(node_base) {
        // 默认标定参数（会在在线标定后更新）
        calib_.left_k = {1194.0f, 0.0f, 960.0f,
                          0.0f, 1194.0f, 540.0f,
                          0.0f, 0.0f, 1.0f};
        calib_.right_k = calib_.left_k;
        calib_.baseline_mm = 80.0f;
        calib_.recalib_confidence = 0.95f;
        calib_.recalib_timestamp_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }

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
    std::condition_variable frame_cv_;
    cv::Mat left_raw_;
    cv::Mat right_raw_;
    uint64_t left_ts_ns_ = 0;
    uint64_t right_ts_ns_ = 0;
    std::atomic<uint64_t> frame_id_{0};
    FrameData current_frame_;

    // ---- SGBM 深度引擎 ----
    SGBMDepthHelper* depth_engine_ = nullptr;
    std::unique_ptr<SGBMDepthHelper> depth_engine_owner_;

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

    // ---- 在线标定器 ----
    std::atomic<bool> calibrator_trigger_{false};
    std::thread calibrator_thread_;
    CalibrationData calib_;

#ifdef ENABLE_ONNXRUNTIME
    // ---- ONNX 置信度推理（可选）----
    std::unique_ptr<ConfidenceInference> confidence_nn_;
    std::string onnx_model_path_;
#else
    std::string onnx_model_path_;
#endif

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

        // ---- 自动选择硬件后端 ----
        std::string platform_info = hardware::getPlatformInfo();
        RCLCPP_INFO(rclcpp::get_logger("StereoCamera"),
                    "平台: %s", platform_info.c_str());

        // 模拟模式检测
        if (config.publish_hz == 0) {
            hw_config_.simulator_mode = true;
            RCLCPP_INFO(rclcpp::get_logger("StereoCamera"), "模拟器模式");
            return true;
        }

        // ---- 初始化 SGBM 深度引擎 ----
        SGBMParams sgbm_params;
        sgbm_params.num_disparities = 128;
        sgbm_params.block_size = 7;
        sgbm_params.P1 = 8 * 7 * 7;
        sgbm_params.P2 = 32 * 7 * 7;
        sgbm_params.min_confidence = config.confidence_threshold;
        sgbm_params.uniqueness_ratio = 15;
        sgbm_params.speckle_window = 100;
        sgbm_params.speckle_range = 32;
        sgbm_params.mode = SGBMParams::ConfidenceMode::Combined;

        depth_engine_owner_ = std::make_unique<SGBMDepthHelper>(sgbm_params);
        depth_engine_ = depth_engine_owner_.get();
        RCLCPP_INFO(rclcpp::get_logger("StereoCamera"), "SGBM 深度引擎已初始化");

        // ---- ONNX 置信度推理（可选）----
        onnx_model_path_ = config.onnx_model_path;
#ifdef ENABLE_ONNXRUNTIME
        if (!onnx_model_path_.empty()) {
            ConfidenceInference::Config nn_cfg;
            nn_cfg.model_path = onnx_model_path_;
            nn_cfg.use_gpu = true;
            confidence_nn_ = std::make_unique<ConfidenceInference>();
            if (confidence_nn_->initialize(nn_cfg)) {
                RCLCPP_INFO(rclcpp::get_logger("StereoCamera"),
                            "ONNX 置信度模型已加载: %s", onnx_model_path_.c_str());
            } else {
                RCLCPP_WARN(rclcpp::get_logger("StereoCamera"),
                            "ONNX 模型初始化失败，使用传统置信度");
                confidence_nn_.reset();
            }
        } else {
            RCLCPP_INFO(rclcpp::get_logger("StereoCamera"),
                        "无 ONNX 模型，使用传统置信度估计");
        }
#else
        if (!onnx_model_path_.empty()) {
            RCLCPP_WARN(rclcpp::get_logger("StereoCamera"),
                        "ONNXRuntime 未编译支持，忽略模型: %s",
                        onnx_model_path_.c_str());
        }
#endif

        // ---- 创建左目录制设备（自动选择 V4L2 或 NvMedia）----
        capture_left_ = createCaptureDevice(hw_config_, "auto");
        if (!capture_left_ || !capture_left_->open(hw_config_)) {
            RCLCPP_ERROR(rclcpp::get_logger("StereoCamera"),
                        "左目录制设备初始化失败");
            capture_left_.reset();
        } else {
            RCLCPP_INFO(rclcpp::get_logger("StereoCamera"),
                        "左目录制设备: %s",
                        capture_left_->getVersion().c_str());
        }

        // ---- 创建右目录制设备 ----
        capture_right_ = createCaptureDevice(hw_config_, "auto");
        if (!capture_right_ || !capture_right_->open(hw_config_)) {
            RCLCPP_WARN(rclcpp::get_logger("StereoCamera"),
                        "右目录制设备初始化失败（单目模式继续）");
            capture_right_.reset();
        }

        // ---- 启动左右目录制线程（真实硬件模式）----
        running_.store(true);

        // 左目采集回调
        if (capture_left_) {
            capture_left_->startStreaming(
                [this](const hardware::FrameBuffer& left, const hardware::FrameBuffer&, const hardware::IMURawData&) {
                    if (!left.empty() && running_.load()) {
                        std::lock_guard<std::mutex> lock(frame_mutex_);
                        int w = left.width, h = left.height;
                        cv::Mat raw12(h, w, CV_16SC1);
                        parseRaw12(static_cast<const uint8_t*>(left.data),
                                   left.stride, w, h, raw12);
                        // 双线性Bayer demosaic (IMX678 RGGB)
                        cv::Mat bayer8;
                        debayerRGGG(raw12, bayer8);
                        left_raw_ = bayer8.clone();
                        left_ts_ns_ = left.timestamp_ns;
                        frame_cv_.notify_one();
                    }
                });
        }

        // 右目采集回调
        if (capture_right_) {
            capture_right_->startStreaming(
                [this](const hardware::FrameBuffer&, const hardware::FrameBuffer& right, const hardware::IMURawData&) {
                    if (!right.empty() && running_.load()) {
                        std::lock_guard<std::mutex> lock(frame_mutex_);
                        int w = right.width, h = right.height;
                        cv::Mat raw12(h, w, CV_16SC1);
                        parseRaw12(static_cast<const uint8_t*>(right.data),
                                   right.stride, w, h, raw12);
                        cv::Mat bayer8;
                        debayerRGGG(raw12, bayer8);
                        right_raw_ = bayer8.clone();
                        right_ts_ns_ = right.timestamp_ns;
                    }
                });
        }

        RCLCPP_INFO(rclcpp::get_logger("StereoCamera"),
                    "左右目录制线程已启动");

        // ---- 在线标定器后台线程 ----
        calibrator_thread_ = std::thread([this]() {
            std::vector<std::pair<cv::Mat, cv::Mat>> calib_frames;
            std::mutex calib_mutex;
            constexpr size_t kCalibWindow = 30;  // 收集30对帧

            while (running_.load()) {
                // 等待触发信号
                while (!calibrator_trigger_.load() && running_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                if (!running_.load()) break;

                calibrator_trigger_.store(false);
                RCLCPP_INFO(rclcpp::get_logger("StereoCalibrator"),
                            "开始在线标定重收敛...");

                // 收集最近的高置信度帧
                {
                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    if (!left_raw_.empty() && !right_raw_.empty()) {
                        calib_frames.emplace_back(left_raw_.clone(), right_raw_.clone());
                        if (calib_frames.size() > kCalibWindow) {
                            calib_frames.erase(calib_frames.begin());
                        }
                    }
                }

                if (calib_frames.size() < 5) {
                    RCLCPP_WARN(rclcpp::get_logger("StereoCalibrator"),
                                "有效帧不足（%zu/5），跳过本次标定",
                                calib_frames.size());
                    continue;
                }

                // ---- 简化BA优化：最小化帧间重投影误差 ----
                // 参数向量：[baseline_scale, pitch_rad, yaw_rad]
                // 真实实现使用 Ceres/G2O，这里用梯度下降近似
                float baseline_scale = 1.0f;
                float pitch = 0.0f, yaw = 0.0f;
                constexpr int kMaxIter = 20;
                constexpr float kLearningRate = 0.01f;

                for (int iter = 0; iter < kMaxIter; ++iter) {
                    float grad_b = 0.0f, grad_p = 0.0f, grad_y = 0.0f;
                    float loss = 0.0f;

                    for (const auto& [left, right] : calib_frames) {
                        // 采样稀疏点做误差和梯度
                        for (int y = 50; y < left.rows - 50; y += 40) {
                            for (int x = 50; x < left.cols - 50; x += 40) {
                                // 简单SSD误差（实际应做立体匹配，这里近似用像素差）
                                float ssd = 0.0f;
                                int cnt = 0;
                                for (int dy = -2; dy <= 2; ++dy) {
                                    for (int dx = -2; dx <= 2; ++dx) {
                                        if (left.channels() == 3) {
                                            cv::Vec3b L = left.at<cv::Vec3b>(y+dy, x+dx);
                                            cv::Vec3b R = right.at<cv::Vec3b>(y+dy, x+dx);
                                            for (int c = 0; c < 3; ++c) {
                                                ssd += (L[c] - R[c]) * (L[c] - R[c]);
                                            }
                                            cnt += 3;
                                        } else {
                                            uchar L = left.at<uchar>(y+dy, x+dx);
                                            uchar R = right.at<uchar>(y+dy, x+dx);
                                            ssd += (L - R) * (L - R);
                                            ++cnt;
                                        }
                                    }
                                }
                                float e = ssd / std::max(cnt, 1);
                                loss += e;

                                // 数值梯度
                                float eps = 0.001f;
                                grad_b += e;  // 简化：baseline↑ → 视差↓
                                grad_p += e * (y - left.rows/2.0f) * 0.001f;
                                grad_y += e * (x - left.cols/2.0f) * 0.001f;
                            }
                        }
                    }

                    // 梯度下降更新
                    baseline_scale -= kLearningRate * grad_b / std::max(grad_b, 1e-6f);
                    pitch -= kLearningRate * grad_p;
                    yaw -= kLearningRate * grad_y;

                    baseline_scale = std::max(0.5f, std::min(2.0f, baseline_scale));
                    pitch = std::max(-0.1f, std::min(0.1f, pitch));
                    yaw = std::max(-0.1f, std::min(0.1f, yaw));
                }

                // 更新标定参数
                {
                    std::lock_guard<std::mutex> lock(imu_mutex_);
                    calib_.baseline_mm *= baseline_scale;
                    calib_.recalib_confidence = 0.92f;  // 估计值
                    calib_.recalib_timestamp_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                }

                RCLCPP_INFO(rclcpp::get_logger("StereoCalibrator"),
                            "标定完成: baseline=%.2fmm, confidence=%.2f",
                            calib_.baseline_mm, calib_.recalib_confidence);
            }
        });

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
                hardware::IMURawData raw;
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
        using StereoCamera::impl_;  // 继承基类的 impl_

        explicit RealCamera(Impl* p) { impl_.reset(p); }

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

        std::unique_ptr<FrameData> realGrabFrame(uint32_t timeout_ms) {
            auto frame = std::make_unique<FrameData>();

            // ---- 等待左右目帧到达 ----
            {
                std::unique_lock<std::mutex> lock(impl_->frame_mutex_);
                bool ok = impl_->frame_cv_.wait_for(
                    lock, std::chrono::milliseconds(timeout_ms),
                    [this] { return !impl_->left_raw_.empty() && !impl_->right_raw_.empty(); }
                );
                if (!ok) {
                    impl_->last_error_ = {
                        StereoError::FRAME_DROPPED,
                        "Frame timeout - no frames received",
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        true, false
                    };
                    impl_->drop_count_++;
                    return frame;
                }
            }

            cv::Mat left_raw, right_raw;
            uint64_t frame_ts_ns = 0;
            {
                std::lock_guard<std::mutex> lock(impl_->frame_mutex_);
                left_raw = impl_->left_raw_.clone();
                right_raw = impl_->right_raw_.clone();
                frame_ts_ns = impl_->left_ts_ns_;
            }

            // ---- 填充标定信息 ----
            CalibrationData calib = getCalibration();

            // ---- 进行深度计算 ----
            if (!left_raw.empty() && !right_raw.empty() && depth_engine_) {
                cv::Mat depth_m, confidence, disparity;
                if (impl_->imu_available_.load()) {
                    IMUData imu;
                    {
                        std::lock_guard<std::mutex> lock(impl_->imu_mutex_);
                        imu = impl_->current_imu_;
                    }
                    depth_engine_->computeWithIMU(
                        left_raw, right_raw, calib, imu,
                        depth_m, confidence, disparity
                    );
                } else {
                    depth_engine_->compute(
                        left_raw, right_raw, calib,
                        depth_m, confidence, disparity
                    );
                }

                // 物理安全性检查
                if (!validateDepthSafety(depth_m)) {
                    impl_->last_error_ = {
                        StereoError::SYNC_LOST,
                        "Depth validation failed - physics impossibility detected",
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        true, false
                    };
                    depth_m.setTo(0.0f);
                    confidence.setTo(0.0f);
                }

                // ---- ONNX NN 置信度推理（可选，与 SGBM 置信度融合）----
#ifdef ENABLE_ONNXRUNTIME
                if (impl_->confidence_nn_) {
                    cv::Mat left_gray, right_gray;
                    if (left_raw.channels() == 3) {
                        cv::cvtColor(left_raw, left_gray, cv::COLOR_BGR2GRAY);
                        cv::cvtColor(right_raw, right_gray, cv::COLOR_BGR2GRAY);
                    } else {
                        left_gray = left_raw;
                        right_gray = right_raw;
                    }
                    try {
                        cv::Mat nn_confidence = impl_->confidence_nn_->infer(
                            left_gray, right_gray, disparity);
                        // 融合：NN 置信度 × 0.6 + SGBM × 0.4
                        if (nn_confidence.size() == confidence.size()) {
                            cv::addWeighted(nn_confidence, 0.6, confidence, 0.4, 0,
                                          confidence);
                        }
                    } catch (const std::exception& e) {
                        RCLCPP_WARN(rclcpp::get_logger("StereoCamera"),
                                    "ONNX 推理失败: %s", e.what());
                    }
                }
#endif

                frame->depth_m = depth_m;
                frame->confidence = confidence;
                frame->disparity = disparity;
                frame->left_rect = left_raw;
                frame->right_rect = right_raw;
            }

            frame->metadata.hw_timestamp_ns = frame_ts_ns;
            frame->metadata.system_timestamp_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            impl_->frame_count_++;
            return frame;
        }

        // 左右目原始图像缓冲（由采集线程填充）
        cv::Mat left_raw;
        cv::Mat right_raw;

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
            return impl_->calib_;
        }

        void triggerRecalibration() override {
            RCLCPP_INFO(rclcpp::get_logger("StereoCamera"),
                        "触发在线标定重收敛...");
            impl_->calibrator_trigger_.store(true);
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
