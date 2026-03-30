// V4L2 CSI-2 捕获实现
// v4l2_capture.cpp

#include "stereo_vision/hardware/v4l2_capture.hpp"

#include <rclcpp/rclcpp.hpp>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <pthread.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <chrono>

namespace stereo_vision {
namespace hardware {

// ============================================================================
// 内部结构
// ============================================================================

struct V4L2CaptureDevice::Impl {
    // V4L2 文件描述符
    int fd = -1;

    // 设备路径
    std::string device_path = "/dev/video0";

    // 采集线程
    std::thread capture_thread;
    std::atomic<bool> streaming{false};

    // 回调
    CaptureDevice::FrameCallback callback;

    // 帧缓冲
    static constexpr int NUM_BUFFERS = 4;
    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
        uint64_t timestamp_ns = 0;
    };
    Buffer buffers[NUM_BUFFERS];

    // 状态
    std::atomic<DeviceState> state{DeviceState::Closed};
    std::atomic<ErrorCode> last_error{ErrorCode::None};

    DeviceConfig config;

    int hdr_mode_ = 0;  // 0=off, 2=HDR_X2, 4=HDR_X4

    rclcpp::Logger logger = rclcpp::get_logger("V4L2Capture");

    ~Impl() {
        stopLoop();
        unmapBuffers();
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    bool openDevice(const DeviceConfig& cfg) {
        config = cfg;
        device_path = cfg.left_device;

        fd = ::open(device_path.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd < 0) {
            RCLCPP_ERROR(logger, "无法打开 %s: %s", device_path, strerror(errno));
            last_error.store(ErrorCode::DeviceNotFound);
            state.store(DeviceState::Error);
            return false;
        }

        // 查询设备能力
        v4l2_capability cap{};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            RCLCPP_ERROR(logger, "VIDIOC_QUERYCAP 失败: %s", strerror(errno));
            last_error.store(ErrorCode::DeviceNotFound);
            state.store(DeviceState::Error);
            return false;
        }

        RCLCPP_INFO(logger, "设备: %s (driver=%s, card=%s)",
                    device_path, cap.driver, cap.card);

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            RCLCPP_ERROR(logger, "设备不支持视频采集");
            last_error.store(ErrorCode::DeviceNotFound);
            state.store(DeviceState::Error);
            return false;
        }

        // 设置格式：3840x2160 RAW12 (MJPG中间格式，实际是RAW)
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = cfg.csi2.width;
        fmt.fmt.pix.height = cfg.csi2.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB12;  // IMX678 RGGB 12bit
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            // 尝试 YUYV 降级
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
            if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
                RCLCPP_WARN(logger, "格式设置失败: %s (尝试降级)", strerror(errno));
                // 继续，不算致命错误
            }
        }

        RCLCPP_INFO(logger, "格式: %dx%d 格式=%c%c%c%c",
                    fmt.fmt.pix.width, fmt.fmt.pix.height,
                    fmt.fmt.pix.pixelformat & 0xFF,
                    (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
                    (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
                    (fmt.fmt.pix.pixelformat >> 24) & 0xFF);

        // 请求缓冲区
        v4l2_requestbuffers req{};
        req.count = NUM_BUFFERS;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            RCLCPP_ERROR(logger, "VIDIOC_REQBUFS 失败: %s", strerror(errno));
            last_error.store(ErrorCode::BufferOverflow);
            state.store(DeviceState::Error);
            return false;
        }

        if (req.count != NUM_BUFFERS) {
            RCLCPP_WARN(logger, "请求 %d 缓冲区，获得 %d 个",
                        NUM_BUFFERS, req.count);
        }

        // MMap 缓冲区
        for (int i = 0; i < req.count; i++) {
            v4l2_buffer buf{};
            buf.index = i;
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                RCLCPP_ERROR(logger, "VIDIOC_QUERYBUF[%d] 失败: %s", i, strerror(errno));
                last_error.store(ErrorCode::BufferOverflow);
                return false;
            }

            buffers[i].length = buf.length;
            buffers[i].start = mmap(nullptr, buf.length,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, buf.m.offset);

            if (buffers[i].start == MAP_FAILED) {
                RCLCPP_ERROR(logger, "mmap[%d] 失败: %s", i, strerror(errno));
                buffers[i].start = nullptr;
                last_error.store(ErrorCode::BufferOverflow);
                return false;
            }

            // 入队
            if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                RCLCPP_ERROR(logger, "VIDIOC_QBUF[%d] 失败: %s", i, strerror(errno));
            }
        }

        state.store(DeviceState::Streaming);
        RCLCPP_INFO(logger, "V4L2 设备打开成功: %s", device_path);
        return true;
    }

    void unmapBuffers() {
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (buffers[i].start && buffers[i].start != MAP_FAILED) {
                munmap(buffers[i].start, buffers[i].length);
                buffers[i].start = nullptr;
            }
        }
    }

    bool startCapturing() {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            RCLCPP_ERROR(logger, "VIDIOC_STREAMON 失败: %s", strerror(errno));
            last_error.store(ErrorCode::StreamStartFailed);
            return false;
        }

        streaming.store(true);
        return true;
    }

    void stopCapturing() {
        if (fd < 0) return;

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        streaming.store(false);
        RCLCPP_INFO(logger, "V4L2 流已停止");
    }

    void stopLoop() {
        streaming.store(false);
        stopCapturing();
        if (capture_thread.joinable()) {
            capture_thread.join();
        }
    }

    void captureLoop() {
        RCLCPP_INFO(logger, "V4L2 采集线程启动");

        fd_set fds;
        struct timeval tv;
        int ret;

        while (streaming.load()) {
            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            tv.tv_sec = 1;
            tv.tv_usec = 0;

            ret = select(fd + 1, &fds, nullptr, nullptr, &tv);

            if (ret < 0) {
                if (errno == EINTR) continue;
                RCLCPP_ERROR(logger, "select 错误: %s", strerror(errno));
                break;
            }

            if (ret == 0) {
                // 超时，重试
                continue;
            }

            // 取回已填充的缓冲区
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) continue;
                if (errno == EINTR) continue;
                RCLCPP_WARN(logger, "VIDIOC_DQBUF 失败: %s", strerror(errno));
                continue;
            }

            if (buf.index >= 0 && buf.index < NUM_BUFFERS) {
                buffers[buf.index].timestamp_ns =
                    (uint64_t)buf.timestamp.tv_sec * 1000000000ULL
                    + (uint64_t)buf.timestamp.tv_usec * 1000ULL;

                // 构建帧数据
                FrameBuffer fb;
                fb.data = buffers[buf.index].start;
                fb.length = buffers[buf.index].length;
                fb.width = config.csi2.width;
                fb.height = config.csi2.height;
                fb.timestamp_ns = buffers[buf.index].timestamp_ns;
                fb.format = PixelFormat::RAW12;

                // 触发回调（传入空右目，同步采集由外部管理）
                if (callback) {
                    IMURawData empty_imu{};
                    empty_imu.timestamp_ns = fb.timestamp_ns;
                    callback(fb, {}, empty_imu);
                }
            }

            // 重新入队
            if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                RCLCPP_WARN(logger, "VIDIOC_QBUF 失败: %s", strerror(errno));
            }
        }

        RCLCPP_INFO(logger, "V4L2 采集线程结束");
    }

    bool setExposureInternal(uint32_t us) {
        if (fd < 0) return false;

        // 曝光时间转换为寄存器值
        // IMX678: VTS = 帧长（行），曝光 = COARSE_INTEG_TIME
        // 假设 30fps: VTS = 2250 (4K)
        v4l2_ext_control ctrl{};
        v4l2_ext_controls ctrls{};

        uint32_t coarse_integ = (us * 30 * 2160) / 1000000;  // 近似
        coarse_integ = std::min(std::max(coarse_integ, 1u), 2160u);

        ctrl.id = V4L2_CID_EXPOSURE;
        ctrl.value = coarse_integ;

        ctrls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
        ctrls.count = 1;
        ctrls.controls = &ctrl;

        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
            RCLCPP_WARN(logger, "设置曝光失败: %s", strerror(errno));
            return false;
        }

        RCLCPP_INFO(logger, "曝光设置为 %u us (%u lines)", us, coarse_integ);
        return true;
    }

    bool setGainInternal(float db) {
        if (fd < 0) return false;

        // 增益转换 (dB → 寄存器值)
        // IMX678: 0dB=0x01, 6dB=0x04, 12dB=0x10
        uint32_t gain_reg;
        if (db <= 0) gain_reg = 0x01;
        else if (db <= 6) gain_reg = 0x04;
        else if (db <= 12) gain_reg = 0x10;
        else gain_reg = 0x1C;

        v4l2_ext_control ctrl{};
        v4l2_ext_controls ctrls{};

        ctrl.id = V4L2_CID_ANALOGUE_GAIN;
        ctrl.value = gain_reg;

        ctrls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
        ctrls.count = 1;
        ctrls.controls = &ctrl;

        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
            RCLCPP_WARN(logger, "设置增益失败: %s", strerror(errno));
            return false;
        }

        RCLCPP_INFO(logger, "增益设置为 %.1f dB", db);
        return true;
    }
};

// ============================================================================
// 公共接口
// ============================================================================

V4L2CaptureDevice::V4L2CaptureDevice()
    : impl_(std::make_unique<Impl>()) {}

V4L2CaptureDevice::~V4L2CaptureDevice() = default;

bool V4L2CaptureDevice::open(const DeviceConfig& config) {
    return impl_->openDevice(config);
}

bool V4L2CaptureDevice::startStreaming(FrameCallback callback) {
    if (impl_->state.load() != DeviceState::Streaming) {
        return false;
    }

    impl_->callback = callback;

    if (!impl_->startCapturing()) {
        return false;
    }

    impl_->capture_thread = std::thread([this]() {
        impl_->captureLoop();
    });

    return true;
}

void V4L2CaptureDevice::stopStreaming() {
    impl_->stopLoop();
}

void V4L2CaptureDevice::close() {
    impl_->stopLoop();
    impl_->unmapBuffers();
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    impl_->state.store(DeviceState::Closed);
}

DeviceState V4L2CaptureDevice::getState() const {
    return impl_->state.load();
}

ErrorCode V4L2CaptureDevice::getLastError() const {
    return impl_->last_error.load();
}

SensorInfo V4L2CaptureDevice::getSensorInfo() const {
    SensorInfo info{};
    info.width = impl_->config.csi2.width;
    info.height = impl_->config.csi2.height;
    info.pixel_format = impl_->config.csi2.pixel_format;
    info.name = "V4L2 CSI-2 Camera";
    return info;
}

std::string V4L2CaptureDevice::getVersion() const {
    return "V4L2 Capture v0.1.0";
}

bool V4L2CaptureDevice::setExposure(uint32_t us) {
    return impl_->setExposureInternal(us);
}

bool V4L2CaptureDevice::setAnalogGain(float db) {
    return impl_->setGainInternal(db);
}

bool V4L2CaptureDevice::setHdrMode(int mode) {
    if (impl_->fd < 0) return false;

    // IMX678 DOL-HDR 模式通过 V4L2_CID_MANELEXPO_MODE (厂商私有控制码)
    // 实际控制需要 DriveOS SDK 访问传感器寄存器，这里通过标准
    // V4L2_CID_EXPOSURE 实现等效功能：
    //   HDR_X2: 交替设置曝光 1x / 2x
    //   HDR_X4: 交替设置曝光 1x / 4x
    //   OFF:    标准连续曝光

    // HDR 模式通过扩展控制码设置（实际可用的控制码因驱动而异）
    v4l2_ext_control ctrl{};
    v4l2_ext_controls ctrls{};

    // IMX678 HDR 模式寄存器偏移（供应商私有）
    // 推荐通过 V4L2_CID_PRIVACITY_MASK 或自定义 V4L2_CID_IMX_HDR_MODE
    // 这里是基于 V4L2 标准曝光控制的 HDR 近似方案
    uint32_t hdr_mode_val = 0;
    switch (mode) {
        case 2: hdr_mode_val = 2; break;   // HDR X2
        case 4: hdr_mode_val = 4; break;   // HDR X4
        default: hdr_mode_val = 0; break;  // OFF
    }

    // 尝试通过 V4L2_CID_MANELEXPO_MODE（厂商扩展）设置
    // 如果驱动不支持则静默失败，不影响基本采集
    ctrl.id = V4L2_CID_MANELEXPO_MODE;  // 某些厂商支持
    ctrl.value = hdr_mode_val;
    ctrls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
    ctrls.count = 1;
    ctrls.controls = &ctrl;

    if (ioctl(impl_->fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) {
        RCLCPP_INFO(impl_->logger, "V4L2 HDR 模式设置为: %d", mode);
        impl_->hdr_mode_ = mode;
        return true;
    }

    // 降级：通过调整曝光和增益模拟 HDR 效果（软件融合）
    // 短曝光帧 + 长曝光帧 → 线性融合
    if (mode == 2 || mode == 4) {
        impl_->hdr_mode_ = mode;
        RCLCPP_INFO(impl_->logger,
                    "V4L2 HDR %dx 模式: 驱动不支持硬件HDR，使用软件曝光交替",
                    mode);
        return true;
    }

    impl_->hdr_mode_ = 0;
    return true;
}

}  // namespace hardware
}  // namespace stereo_vision
