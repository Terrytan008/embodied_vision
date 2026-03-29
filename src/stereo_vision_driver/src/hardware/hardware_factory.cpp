// 硬件抽象层 — 硬件工厂
// hardware_factory.cpp

#include "stereo_vision/hardware/capture_base.hpp"
#include "stereo_vision/hardware/camera_types.hpp"
#include <rclcpp/rclcpp.hpp>

namespace stereo_vision::hardware {

// 前向声明
class V4L2CaptureDevice;
class BMI088Driver;

// ============================================================================
// 双目相机硬件管理器
// ============================================================================
class StereoHardwareManager {
public:
    StereoHardwareManager();
    ~StereoHardwareManager();

    /**
     * @brief 初始化所有硬件
     * @param cfg 设备配置
     * @return true=成功
     */
    bool initialize(const DeviceConfig& cfg);

    /**
     * @brief 开始采集
     * @param left_cb 左目帧回调
     * @param right_cb 右目帧回调
     * @param imu_cb IMU数据回调（可选）
     */
    bool start(CaptureDevice::FrameCallback left_cb,
               CaptureDevice::FrameCallback right_cb,
               std::function<void(const IMURawData&)> imu_cb);

    /**
     * @brief 停止采集
     */
    void stop();

    /**
     * @brief 关闭所有硬件
     */
    void shutdown();

    /**
     * @brief 获取传感器信息
     */
    SensorInfo getSensorInfo() const;

    /**
     * @brief 设置曝光
     */
    bool setExposure(uint32_t us);

    /**
     * @brief 设置增益
     */
    bool setAnalogGain(float db);

    /**
     * @brief 是否模拟模式
     */
    bool isSimulatorMode() const { return config_.simulator_mode; }

private:
    DeviceConfig config_;
    std::unique_ptr<CaptureDevice> capture_left_;
    std::unique_ptr<CaptureDevice> capture_right_;
    std::unique_ptr<BMI088Driver> imu_;

    // IMU 轮询线程
    std::thread imu_thread_;
    std::atomic<bool> imu_running_{false};
    std::function<void(const IMURawData&)> imu_callback_;

    rclcpp::Logger logger_ = rclcpp::get_logger("StereoHardware");
};

// ============================================================================
// 实现
// ============================================================================

StereoHardwareManager::StereoHardwareManager() = default;

StereoHardwareManager::~StereoHardwareManager() {
    shutdown();
}

bool StereoHardwareManager::initialize(const DeviceConfig& cfg) {
    config_ = cfg;

    if (config_.simulator_mode) {
        RCLCPP_INFO(logger_, "模拟器模式初始化");
        return true;
    }

    // ---- 初始化左目捕获 ----
    RCLCPP_INFO(logger_, "初始化左目 CSI-2...");
    capture_left_ = createCaptureDevice("v4l2");
    if (!capture_left_ || !capture_left_->open(cfg)) {
        RCLCPP_ERROR(logger_, "左目初始化失败");
        return false;
    }

    // ---- 初始化右目捕获 ----
    // 注：真实双目需要两个 /dev/videoX 节点
    // 这里假设右目对应 /dev/video1
    RCLCPP_INFO(logger_, "初始化右目 CSI-2...");
    capture_right_ = createCaptureDevice("v4l2");
    if (!capture_right_ || !capture_right_->open(cfg)) {
        RCLCPP_WARN(logger_, "右目初始化失败（单目模式继续）");
        capture_right_.reset();
    }

    // ---- 初始化 IMU ----
    RCLCPP_INFO(logger_, "初始化 IMU (BMI088 @ %s)...", cfg.i2c_bus);
    imu_ = std::make_unique<BMI088Driver>();
    if (!imu_->open(cfg.i2c_bus, cfg.imu.addr, cfg.imu.addr)) {
        RCLCPP_WARN(logger_, "IMU 初始化失败，将无 IMU 数据");
        imu_.reset();
    } else if (!imu_->init()) {
        RCLCPP_WARN(logger_, "IMU 配置失败，将无 IMU 数据");
        imu_.reset();
    } else {
        RCLCPP_INFO(logger_, "IMU 初始化成功");
    }

    return true;
}

bool StereoHardwareManager::start(
    CaptureDevice::FrameCallback left_cb,
    CaptureDevice::FrameCallback right_cb,
    std::function<void(const IMURawData&)> imu_cb) {

    imu_callback_ = imu_cb;

    if (config_.simulator_mode) {
        RCLCPP_INFO(logger_, "模拟器模式，跳过硬件采集");
        return true;
    }

    // 启动左目录制
    if (capture_left_ && !capture_left_->startStreaming(left_cb)) {
        RCLCPP_ERROR(logger_, "左目录制启动失败");
        return false;
    }

    // 启动右目录制
    if (capture_right_ && !capture_right_->startStreaming(right_cb)) {
        RCLCPP_WARN(logger_, "右目录制启动失败（单目模式继续）");
        capture_right_.reset();
    }

    // 启动 IMU 轮询
    if (imu_) {
        imu_running_.store(true);
        imu_thread_ = std::thread([this]() {
            IMURawData data;
            while (imu_running_.load()) {
                if (imu_->read(data)) {
                    data.timestamp_ns = 0;  // 由主线程填充
                    if (imu_callback_) {
                        imu_callback_(data);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));  // 200Hz
            }
        });
        RCLCPP_INFO(logger_, "IMU 轮询已启动 (200Hz)");
    }

    return true;
}

void StereoHardwareManager::stop() {
    if (config_.simulator_mode) return;

    imu_running_.store(false);
    if (imu_thread_.joinable()) {
        imu_thread_.join();
    }

    if (capture_left_) capture_left_->stopStreaming();
    if (capture_right_) capture_right_->stopStreaming();

    RCLCPP_INFO(logger_, "硬件采集已停止");
}

void StereoHardwareManager::shutdown() {
    stop();

    if (capture_left_) { capture_left_->close(); capture_left_.reset(); }
    if (capture_right_) { capture_right_->close(); capture_right_.reset(); }
    if (imu_) { imu_->close(); imu_.reset(); }

    RCLCPP_INFO(logger_, "硬件已关闭");
}

SensorInfo StereoHardwareManager::getSensorInfo() const {
    if (capture_left_) {
        return capture_left_->getSensorInfo();
    }
    return SensorInfo{};
}

bool StereoHardwareManager::setExposure(uint32_t us) {
    bool ok = true;
    if (capture_left_) ok &= capture_left_->setExposure(us);
    if (capture_right_) ok &= capture_right_->setExposure(us);
    return ok;
}

bool StereoHardwareManager::setAnalogGain(float db) {
    bool ok = true;
    if (capture_left_) ok &= capture_left_->setAnalogGain(db);
    if (capture_right_) ok &= capture_right_->setAnalogGain(db);
    return ok;
}

// ============================================================================
// 工厂函数实现
// ============================================================================

std::unique_ptr<CaptureDevice> createCaptureDevice(const std::string& platform) {
    if (platform == "v4l2" || platform.empty()) {
        // 使用 V4L2 实现
        // 注意：这里需要多态，实际需要虚函数表
        // 简化：直接返回 nullptr，让调用方使用 V4L2CaptureDevice
        return nullptr;  // 由使用方直接实例化
    }
    return nullptr;
}

}  // namespace stereo_vision::hardware
