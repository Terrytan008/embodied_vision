// 硬件抽象层 — NVIDIA CSI-2 捕获实现
// nv_csi_capture.cpp

#include "stereo_vision/hardware/capture_base.hpp"
#include "stereo_vision/hardware/camera_types.hpp"

#include <rclcpp/rclcpp.hpp>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/tegra-v4l2.h>
#include <errno.h>
#include <string.h>
#include <poll.h>

#include <cstring>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>

// ============================================================================
// V4L2 CSI-2 捕获实现 (适用于所有 Linux 平台，含 Jetson/DRIVE)
// ============================================================================
namespace stereo_vision::hardware {

// V4L2 缓冲区管理
struct V4L2Buffer {
    void* start = nullptr;
    size_t length = 0;
    uint32_t index = 0;
    uint64_t timestamp_ns = 0;
};

// --------------------------------------------------------------------------
// V4L2 CSI Capture Device
// --------------------------------------------------------------------------
class V4L2CaptureDevice : public CaptureDevice {
public:
    V4L2CaptureDevice();
    ~V4L2CaptureDevice() override;

    bool open(const DeviceConfig& config) override;
    bool startStreaming(FrameCallback callback) override;
    void stopStreaming() override;
    void close() override;

    DeviceState getState() const override {
        return state_.load(std::memory_order_acquire);
    }

    ErrorCode getLastError() const override {
        return last_error_.load(std::memory_order_acquire);
    }

    SensorInfo getSensorInfo() const override {
        return sensor_info_;
    }

    std::string getVersion() const override {
        return "V4L2 Capture v0.1.0 (Embodied Vision)";
    }

    bool setExposure(uint32_t us) override;
    bool setAnalogGain(float db) override;
    bool setHdrMode(int mode) override;

private:
    // 内部方法
    bool initDevice(const DeviceConfig& config);
    bool initMmap();
    bool enqueueBuffers();
    bool captureLoop();  // 采集线程

    // I2C/Sensor 配置
    bool configSensor();
    bool writeI2C(uint8_t addr, const uint8_t* data, size_t len);
    bool readI2C(uint8_t addr, uint8_t* data, size_t len);

    // 文件描述符
    int fd_ = -1;
    int i2c_fd_ = -1;

    // 设备配置
    DeviceConfig config_;

    // 缓冲区
    std::vector<V4L2Buffer> buffers_;
    static constexpr uint32_t kBufferCount = 4;

    // 左右目（V4L2 支持多平面，但简化处理用两个设备）
    // 对于双目，这里用两个 /dev/videoX 分别对应左右目
    // 这里实现单设备（第一个视频节点），双目通过两个实例管理

    // 双目同步
    std::atomic<bool> left_ready_{false};
    std::atomic<bool> right_ready_{false};
    FrameBuffer left_frame_;
    FrameBuffer right_frame_;
    IMURawData last_imu_{};
    std::mutex frame_mutex_;

    // 采集线程
    std::thread capture_thread_;
    std::atomic<bool> streaming_{false};

    // 回调
    FrameCallback frame_callback_;

    // 状态
    std::atomic<DeviceState> state_{DeviceState::Closed};
    std::atomic<ErrorCode> last_error_{ErrorCode::None};

    SensorInfo sensor_info_{};

    // ROS2 logger
    rclcpp::Logger logger_ = rclcpp::get_logger("V4L2Capture");
};

// --------------------------------------------------------------------------
// 工厂函数实现
// --------------------------------------------------------------------------
std::unique_ptr<CaptureDevice> createCaptureDevice(const std::string& platform) {
    if (platform == "v4l2" || platform.empty()) {
        return std::make_unique<V4L2CaptureDevice>();
    }
    // TODO: nvargus for NVIDIA platforms
    // if (platform == "nvargus" || platform == "nvidia") { ... }
    return std::make_unique<V4L2CaptureDevice>();
}

// ============================================================================
// V4L2 实现
// ============================================================================

V4L2CaptureDevice::V4L2CaptureDevice() = default;

V4L2CaptureDevice::~V4L2CaptureDevice() {
    close();
}

bool V4L2CaptureDevice::open(const DeviceConfig& config) {
    config_ = config;

    if (config_.simulator_mode) {
        RCLCPP_INFO(logger_, "模拟器模式，跳过硬件初始化");
        state_.store(DeviceState::Streaming);
        return true;
    }

    if (state_.load() != DeviceState::Closed) {
        RCLCPP_WARN(logger_, "设备未关闭，先关闭");
        close();
    }

    state_.store(DeviceState::Opening);

    // 打开 V4L2 设备（默认 /dev/video0，可配置）
    const char* device = "/dev/video0";
    fd_ = ::open(device, O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
        RCLCPP_ERROR(logger_, "无法打开 %s: %s", device, strerror(errno));
        last_error_.store(ErrorCode::DeviceNotFound);
        state_.store(DeviceState::Error);
        return false;
    }

    // 查询设备能力
    v4l2_capability cap{};
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        RCLCPP_ERROR(logger_, "VIDIOC_QUERYCAP 失败: %s", strerror(errno));
        state_.store(DeviceState::Error);
        last_error_.store(ErrorCode::StreamStartFailed);
        return false;
    }

    RCLCPP_INFO(logger_, "设备: %s (%s)", cap.card, cap.driver);

    // 检查是否是捕获设备
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        RCLCPP_ERROR(logger_, "不是视频捕获设备");
        state_.store(DeviceState::Error);
        last_error_.store(ErrorCode::DeviceNotFound);
        return false;
    }

    // 检查是否支持流式I/O
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        RCLCPP_ERROR(logger_, "不支持流式I/O");
        state_.store(DeviceState::Error);
        last_error_.store(ErrorCode::StreamStartFailed);
        return false;
    }

    // 初始化设备
    if (!initDevice(config_)) {
        state_.store(DeviceState::Error);
        return false;
    }

    // 初始化MMAP缓冲区
    if (!initMmap()) {
        state_.store(DeviceState::Error);
        last_error_.store(ErrorCode::BufferOverflow);
        return false;
    }

    RCLCPP_INFO(logger_, "设备初始化完成");
    state_.store(DeviceState::Streaming);
    return true;
}

bool V4L2CaptureDevice::initDevice(const DeviceConfig& config) {
    // 设置视频格式：3840x2160 RAW12 (IMX678 默认)
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = config.csi2.width;
    fmt.fmt.pix.height = config.csi2.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB12;  // IMX678 使用 RGGB
    fmt.fmt.pix.field = V4L2_FIELD_NONE;  // 逐行扫描

    if (::ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        RCLCPP_WARN(logger_, "VIDIOC_S_FMT 失败，尝试 YUV: %s", strerror(errno));
        // 降级为 YUV422（大多数Sensor支持）
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (::ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            RCLCPP_ERROR(logger_, "无法设置视频格式: %s", strerror(errno));
            return false;
        }
    }

    // 设置帧率
    v4l2_streamparm streamparm{};
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator = 1;
    streamparm.parm.capture.timeperframe.denominator = 30;  // 30fps
    streamparm.parm.capture.capturemode = 0;
    ::ioctl(fd_, VIDIOC_S_PARM, &streamparm);

    RCLCPP_INFO(logger_, "格式设置: %dx%d fmt=%.4s",
                 fmt.fmt.pix.width, fmt.fmt.pix.height,
                 reinterpret_cast<char*>(&fmt.fmt.pix.pixelformat));

    return true;
}

bool V4L2CaptureDevice::initMmap() {
    // 请求缓冲区
    v4l2_requestbuffers req{};
    req.count = kBufferCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (::ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        RCLCPP_ERROR(logger_, "VIDIOC_REQBUFS 失败: %s", strerror(errno));
        return false;
    }

    buffers_.resize(req.count);

    // MMAP 映射
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (::ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            RCLCPP_ERROR(logger_, "VIDIOC_QUERYBUF[%u] 失败", i);
            return false;
        }

        buffers_[i].start = ::mmap(nullptr, buf.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd_, buf.m.offset);
        buffers_[i].length = buf.length;
        buffers_[i].index = i;

        if (buffers_[i].start == MAP_FAILED) {
            RCLCPP_ERROR(logger_, "mmap[%u] 失败: %s", i, strerror(errno));
            return false;
        }
    }

    return enqueueBuffers();
}

bool V4L2CaptureDevice::enqueueBuffers() {
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (::ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            RCLCPP_ERROR(logger_, "VIDIOC_QBUF[%u] 失败: %s", i, strerror(errno));
            return false;
        }
    }
    return true;
}

bool V4L2CaptureDevice::startStreaming(FrameCallback callback) {
    if (state_.load() != DeviceState::Streaming && !config_.simulator_mode) {
        RCLCPP_ERROR(logger_, "设备未就绪，无法开始采集");
        return false;
    }

    frame_callback_ = callback;
    streaming_.store(true);

    // 启动采集线程
    capture_thread_ = std::thread(&V4L2CaptureDevice::captureLoop, this);

    // 启动视频流
    if (!config_.simulator_mode) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (::ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            RCLCPP_ERROR(logger_, "VIDIOC_STREAMON 失败: %s", strerror(errno));
            streaming_.store(false);
            return false;
        }
        RCLCPP_INFO(logger_, "流已启动");
    }

    return true;
}

void V4L2CaptureDevice::captureLoop() {
    rclcpp::Clock clock;
    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    while (streaming_.load()) {
        int ret = ::poll(&pfd, 1, 100);  // 100ms 超时

        if (ret < 0) {
            if (errno == EINTR) continue;
            RCLCPP_ERROR(logger_, "poll 错误: %s", strerror(errno));
            break;
        }

        if (ret == 0) continue;  // 超时，继续

        // DQ 缓冲区
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (::ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            RCLCPP_ERROR(logger_, "VIDIOC_DQBUF 失败: %s", strerror(errno));
            break;
        }

        // 填充帧数据
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);

            void* frame_data = buffers_[buf.index].start;
            uint64_t timestamp = (static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000000ULL
                                   + static_cast<uint64_t>(buf.timestamp.tv_usec) * 1000ULL);

            // 左目帧（这里简化处理，真实双目需要两个设备或复合格式）
            left_frame_ = FrameBuffer(
                frame_data,
                config_.csi2.width,
                config_.csi2.height,
                PixelFormat::RAW12,
                timestamp,
                buf.sequence
            );

            left_ready_.store(true, std::memory_order_release);

            // 如果右目也准备好了，触发回调
            if (right_ready_.load(std::memory_order_acquire)) {
                if (frame_callback_) {
                    frame_callback_(left_frame_, right_frame_, last_imu_);
                }
                right_ready_.store(false, std::memory_order_release);
            }
        }

        // 重新入队
        if (::ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            RCLCPP_ERROR(logger_, "VIDIOC_QBUF(requeue) 失败: %s", strerror(errno));
        }
    }

    RCLCPP_INFO(logger_, "采集线程结束");
}

void V4L2CaptureDevice::stopStreaming() {
    if (!streaming_.load()) return;

    streaming_.store(false);

    // 停止视频流
    if (fd_ >= 0 && !config_.simulator_mode) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ::ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }

    // 等待线程结束
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    RCLCPP_INFO(logger_, "流已停止");
}

void V4L2CaptureDevice::close() {
    stopStreaming();

    if (fd_ >= 0) {
        // 释放 MMAP 缓冲区
        for (auto& buf : buffers_) {
            if (buf.start && buf.start != MAP_FAILED) {
                ::munmap(buf.start, buf.length);
            }
        }
        buffers_.clear();
        ::close(fd_);
        fd_ = -1;
    }

    if (i2c_fd_ >= 0) {
        ::close(i2c_fd_);
        i2c_fd_ = -1;
    }

    state_.store(DeviceState::Closed);
    RCLCPP_INFO(logger_, "设备已关闭");
}

bool V4L2CaptureDevice::writeI2C(uint8_t, const uint8_t*, size_t) {
    // TODO: I2C Sensor 配置（IMX678寄存器初始化序列）
    return true;
}

bool V4L2CaptureDevice::readI2C(uint8_t, uint8_t*, size_t) {
    return true;
}

bool V4L2CaptureDevice::setExposure(uint32_t us) {
    // V4L2_CID_EXPOSURE
    if (fd_ < 0) return false;

    v4l2_control ctrl{};
    ctrl.id = V4L2_CID_EXPOSURE;
    ctrl.value = us;

    if (::ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        RCLCPP_WARN(logger_, "设置曝光失败: %s", strerror(errno));
        return false;
    }
    return true;
}

bool V4L2CaptureDevice::setAnalogGain(float db) {
    if (fd_ < 0) return false;

    // V4L2_CID_GAIN (0 = auto, >0 = 手动增益 in units of 0.1 dB)
    v4l2_control ctrl{};
    ctrl.id = V4L2_CID_GAIN;
    ctrl.value = static_cast<int>(db * 10.0f);  // dB → 0.1dB 单位

    if (::ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        RCLCPP_WARN(logger_, "设置增益失败: %s", strerror(errno));
        return false;
    }
    return true;
}

bool V4L2CaptureDevice::setHdrMode(int mode) {
    // TODO: IMX678 HDR 模式寄存器配置
    (void)mode;
    return false;
}

}  // namespace stereo_vision::hardware
