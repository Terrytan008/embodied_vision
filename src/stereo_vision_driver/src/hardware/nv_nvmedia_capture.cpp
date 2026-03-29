// Embodied Vision — NVIDIA NvMedia CSI-2 原生捕获实现
// nv_nvmedia_capture.cpp
//
// 适用于：NVIDIA DRIVE AGX Thor / Jetson AGX Orin / Jetson NX
// 使用 NvMedia API 直接访问 CSI-2 端口，实现零拷贝 GPU 内存传递
//
// NvMedia 数据流：
//   CSI-Sensor → NvMedia IJP → NvMedia Image → CUDA GPU Memory
//              (内嵌 ISP 处理)       (零拷贝)

#include "stereo_vision/hardware/capture_base.hpp"
#include "stereo_vision/hardware/camera_types.hpp"

#include <rclcpp/rclcpp.hpp>

#include <cuda_runtime.h>
#include <cudaEGL.h>

// NvMedia 头文件（DRIVE OS / JetPack SDK）
// 注意：这些头文件需要 NVIDIA DriveOS SDK 或 JetPack 安装
#include <nvmedia.h>
#include <nvmedia_ijp.h>
#include <nvmedia_surface.h>
#include <nvmedia_image.h>
#include <nvmedia_bufpool.h>
#include <nvmedia_device.h>

#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <memory>
#include <cstring>

// ============================================================================
// NvMedia 辅助函数
// ============================================================================

namespace {

// NvMedia → CUDA 内存类型映射
inline cudaEGLFrameType getCudaEGLFrameType(NvMediaSurfaceType surf_type) {
    switch (surf_type) {
        case NvMediaSurfaceType_Image_Generic:    return cudaEGLFrameTypePitch;
        case NvMediaSurfaceType_Image_YUV_ER:      return cudaEGLFrameTypePitch;
        default:                                    return cudaEGLFrameTypePitch;
    }
}

// CUDA EGL 初始化（必须在主线程调用一次）
class CudaEGLInitializer {
public:
    static bool init() {
        static CudaEGLInitializer inst;
        return inst.ok_;
    }

private:
    CudaEGLInitializer() {
        // CUDA 初始化
        cudaFree(0);  // 触发 CUDA context 创建

        // EGL 初始化（NvMedia 需要 EGL 才能创建 surface）
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY) {
            RCLCPP_ERROR(rclcpp::get_logger("NvMedia"),
                        "EGL display 获取失败");
            ok_ = false;
            return;
        }

        EGLint major, minor;
        if (!eglInitialize(display_, &major, &minor)) {
            RCLCPP_ERROR(rclcpp::get_logger("NvMedia"),
                        "EGL 初始化失败");
            ok_ = false;
            return;
        }

        RCLCPP_INFO(rclcpp::get_logger("NvMedia"),
                    "EGL %d.%d 初始化成功", major, minor);
        ok_ = true;
    }

    ~CudaEGLInitializer() {
        if (display_ != EGL_NO_DISPLAY) {
            eglTerminate(display_);
        }
    }

    bool ok_ = false;
    EGLDisplay display_ = EGL_NO_DISPLAY;
};

}  // anonymous namespace

// ============================================================================
// NvMedia CSI-2 捕获设备
// ============================================================================

class NvMediaCaptureDevice : public CaptureDevice {
public:
    NvMediaCaptureDevice();
    ~NvMediaCaptureDevice() override;

    // ---- CaptureDevice 接口 ----
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
        return "NvMedia CSI-2 Capture v0.1.0 (Embodied Vision)";
    }

    bool setExposure(uint32_t us) override;
    bool setAnalogGain(float db) override;
    bool setHdrMode(int mode) override;

private:
    // ---- NvMedia 内部 ----
    bool initNvMedia();
    bool initIJP();        // Image Jet Processor (ISP)
    bool createBufferPool();
    bool captureLoop();     // 采集线程

    // I2C/Sensor 配置
    bool configSensorIMX678();
    bool writeI2C(uint8_t reg, uint16_t value);
    uint16_t readI2C(uint8_t reg);

    // ---- 成员变量 ----
    DeviceConfig config_;

    // NvMedia 对象
    NvMediaDevice* device_ = nullptr;
    NvMediaIJPObj* ijp_ = nullptr;        // ISP/IJP 对象
    NvMediaBufPool* buf_pool_ = nullptr;   // 缓冲区池
    NvMediaImage* surfaces_ = nullptr;     // NvMedia surfaces

    // CUDA EGL
    bool egl_initialized_ = false;

    // 采集线程
    std::thread capture_thread_;
    std::atomic<bool> streaming_{false};

    // 回调
    FrameCallback frame_callback_;

    // 帧缓冲队列
    static constexpr uint32_t kQueueSize = 4;
    struct FrameQueueItem {
        FrameBuffer frame;
        uint64_t timestamp_ns;
        bool valid;
    };
    FrameQueueItem frame_queue_[kQueueSize];
    std::atomic<uint32_t> write_idx_{0};
    std::atomic<uint32_t> read_idx_{0};
    std::mutex queue_mutex_;
    pthread_cond_t queue_cond_;
    pthread_mutex_t queue_pthread_mutex_;

    // 状态/错误
    std::atomic<DeviceState> state_{DeviceState::Closed};
    std::atomic<ErrorCode> last_error_{ErrorCode::None};
    SensorInfo sensor_info_{};

    // 文件描述符
    int i2c_fd_ = -1;

    rclcpp::Logger logger_ = rclcpp::get_logger("NvMediaCapture");
};

// ============================================================================
// 实现
// ============================================================================

NvMediaCaptureDevice::NvMediaCaptureDevice() {
    pthread_mutex_init(&queue_pthread_mutex_, nullptr);
}

NvMediaCaptureDevice::~NvMediaCaptureDevice() {
    close();
    pthread_mutex_destroy(&queue_pthread_mutex_);
}

bool NvMediaCaptureDevice::initNvMedia() {
    // ---- 创建设备 ----
    NvMediaStatus status = NvMediaDeviceCreate(
        0,  // 设备索引（0=主设备）
        &device_
    );

    if (status != NVMEDIA_STATUS_OK || !device_) {
        RCLCPP_ERROR(logger_, "NvMediaDeviceCreate 失败: %d", status);
        last_error_.store(ErrorCode::DeviceNotFound);
        return false;
    }

    RCLCPP_INFO(logger_, "NvMedia 设备创建成功");
    return true;
}

bool NvMediaCaptureDevice::initIJP() {
    // ---- 创建 IJP (Image Jet Processor) 对象 ----
    // IJP = NvMedia 的 ISP 实现，负责：
    //   - RAW → RGB/YCbCr 转换
    //   - 坏点校正
    //   - 去噪 / HDR 融合
    //   - 色彩校正

    NvMediaIJPConfiguration ijp_config{};
    ijp_config.CsiPort = static_cast<NvMediaCsiPort>(config_.csi2.port);
    ijp_config.CsiLanes = config_.csi2.lanes;
    ijp_config.inputFormat = NvMediaSurfaceType_Image_RAW;

    // IMX678 使用 RGGB Bayer 格式
    switch (config_.csi2.pixel_format) {
        case PixelFormat::RAW10:
            ijp_config.bayerAlgo = NVMEDIA_BAYER_ALGO_ID(
                NVMEDIA_RAW_CHANNEL_RGGB,
                NVMEDIA_BAYER_DYNAMIC_RANGE_HDR,
                NVMEDIA_BAYER_ALGORITHM_LINEAR
            );
            ijp_config.inputBitDepth = 10;
            break;
        case PixelFormat::RAW12:
            ijp_config.bayerAlgo = NVMEDIA_BAYER_ALGO_ID(
                NVMEDIA_RAW_CHANNEL_RGGB,
                NVMEDIA_BAYER_DYNAMIC_RANGE_HDR,
                NVMEDIA_BAYER_ALGORITHM_LINEAR
            );
            ijp_config.inputBitDepth = 12;
            break;
        default:
            ijp_config.inputBitDepth = 12;
    }

    // 输出格式：RGBA（4通道，用于 CUDA 零拷贝）
    ijp_config.outputFormat = NvMediaSurfaceType_Image_RGBA;
    ijp_config.outputBitDepth = 8;

    // 分辨率
    ijp_config.width = config_.csi2.width;
    ijp_config.height = config_.csi2.height;

    // 帧率
    ijp_config.frameRate = 30;  // 30fps

    NvMediaStatus status = NvMediaIJPObjectsCreate(
        device_,
        &ijp_config,
        1,  // num Surfaces
        kQueueSize,  // 缓冲区数量
        &ijp_
    );

    if (status != NVMEDIA_STATUS_OK || !ijp_) {
        RCLCPP_ERROR(logger_, "NvMediaIJPObjectsCreate 失败: %d", status);
        last_error_.store(ErrorCode::StreamStartFailed);
        return false;
    }

    RCLCPP_INFO(logger_, "IJP 创建成功: %dx%d %d-lane CSI-%d",
                config_.csi2.width, config_.csi2.height,
                config_.csi2.lanes, config_.csi2.port);
    return true;
}

bool NvMediaCaptureDevice::createBufferPool() {
    // ---- 创建 NvMedia 缓冲区池 ----
    // 分配 surfaces 用于存储 IJP 输出

    NvMediaBufPoolCreateParams pool_params{};
    pool_params.attributes = NvMediaBufAttr_Create_Image;
    pool_params.poolSize = kQueueSize;

    // Surface 属性
    NvMediaSurfaceAttributes surf_attrs{};
    surf_attrs.type = NvMediaSurfaceType_Image_RGBA;
    surf_attrs.width = config_.csi2.width;
    surf_attrs.height = config_.csi2.height;
    surf_attrs.surfacesAllocated = kQueueSize;

    pool_params.surfaceAttributes = &surf_attrs;

    NvMediaStatus status = NvMediaBufPoolCreate(
        device_,
        &pool_params,
        kQueueSize,
        &buf_pool_
    );

    if (status != NVMEDIA_STATUS_OK) {
        RCLCPP_ERROR(logger_, "NvMediaBufPoolCreate 失败: %d", status);
        last_error_.store(ErrorCode::BufferOverflow);
        return false;
    }

    RCLCPP_INFO(logger_, "缓冲区池创建成功 (%d x %dx%d RGBA)",
                kQueueSize, config_.csi2.width, config_.csi2.height);
    return true;
}

bool NvMediaCaptureDevice::configSensorIMX678() {
    // ---- IMX678 传感器寄存器配置 ----
    // 以下寄存器序列基于 IMX678 数据手册

    // 1. 软件复位
    if (!writeI2C(0x0100, 0x00)) return false;
    usleep(10000);

    // 2. 模式配置：4K 30fps RAW12 4-lane CSI
    // PLL 时钟配置
    if (!writeI2C(0x0301, 0x06)) return false;  // PLL_DIV_REG
    if (!writeI2C(0x0303, 0x04)) return false;  // PLL_PREDIV_REG
    if (!writeI2C(0x0305, 0x04)) return false;  // PLL_MULT_REG (4x)
    if (!writeI2C(0x0309, 0x05)) return false;  // CSI_DIV_REG

    // 3. 图像尺寸
    if (!writeI2C(0x0340, 0x08)) return false;  // VS_LEAD
    if (!writeI2C(0x0341, 0x70)) return false;
    if (!writeI2C(0x0342, 0x17)) return false;  // HS_TRAIL
    if (!writeI2C(0x0343, 0x70)) return false;

    // 4. 窗口编程
    if (!writeI2C(0x0344, 0x00)) return false;  // X_ADDR_START
    if (!writeI2C(0x0345, 0x00)) return false;
    if (!writeI2C(0x0346, 0x00)) return false;  // Y_ADDR_START
    if (!writeI2C(0x0347, 0x00)) return false;

    // H 输出: 3840 像素
    if (!writeI2C(0x0348, 0x0F)) return false;  // X_ADDR_END (H-1)
    if (!writeI2C(0x0349, 0x1F)) return false;
    // V 输出: 2160 行
    if (!writeI2C(0x034A, 0x08)) return false;  // Y_ADDR_END (V-1)
    if (!writeI2C(0x034B, 0x6F)) return false;

    // 5. 模拟增益配置
    if (!writeI2C(0x0204, 0x01)) return false;  // ANALOG_GAIN (0dB)
    if (!writeI2C(0x0205, 0x00)) return false;

    // 6. 曝光配置（行曝光）
    if (!writeI2C(0x0202, 0x03)) return false;  // COARSE_INTEG_TIME_H
    if (!writeI2C(0x0203, 0xE8)) return false;  // COARSE_INTEG_TIME_L (~1000行)

    // 7. CSI-2 输出配置
    if (!writeI2C(0x0112, 0x0A)) return false;  // CSI输出格式: RAW12 (0x0A)
    if (!writeI2C(0x0113, 0x0A)) return false;  // CSI通道分配

    // 8. 数字感兴趣区域（感兴趣区域缩放）
    if (!writeI2C(0x0401, 0x00)) return false;  // DIG_CROP_MODE
    if (!writeI2C(0x0403, 0x00)) return false;
    if (!writeI2C(0x0404, 0x10)) return false;  // DIG_CROP_X_START
    if (!writeI2C(0x0405, 0x00)) return false;
    if (!writeI2C(0x0406, 0x0E)) return false;  // DIG_CROP_X_END
    if (!writeI2C(0x0407, 0xFF)) return false;
    if (!writeI2C(0x0408, 0x07)) return false;  // DIG_CROP_Y_START
    if (!writeI2C(0x0409, 0x68)) return false;
    if (!writeI2C(0x040A, 0x08)) return false;  // DIG_CROP_Y_END
    if (!writeI2C(0x040B, 0x6F)) return false;

    // 9. 使能流输出
    if (!writeI2C(0x0100, 0x01)) return false;

    RCLCPP_INFO(logger_, "IMX678 寄存器配置完成");
    return true;
}

bool NvMediaCaptureDevice::writeI2C(uint8_t reg, uint16_t value) {
    if (i2c_fd_ < 0) return false;

    // IMX678 I2C 格式：高字节reg地址 + 低字节值
    uint8_t buf[2] = {static_cast<uint8_t>(reg), static_cast<uint8_t>(value & 0xFF)};
    // （注：IMX678使用16位寄存器地址，实际需根据datasheet调整）

    if (::write(i2c_fd_, buf, 2) != 2) {
        RCLCPP_WARN(logger_, "I2C write 失败: reg=0x%02X", reg);
        return false;
    }
    return true;
}

uint16_t NvMediaCaptureDevice::readI2C(uint8_t reg) {
    if (i2c_fd_ < 0) return 0;

    uint8_t reg_addr = reg;
    if (::write(i2c_fd_, &reg_addr, 1) != 1) return 0;

    uint8_t value = 0;
    if (::read(i2c_fd_, &value, 1) != 1) return 0;
    return value;
}

bool NvMediaCaptureDevice::open(const DeviceConfig& config) {
    config_ = config;
    state_.store(DeviceState::Opening);

    if (!CudaEGLInitializer::init()) {
        RCLCPP_ERROR(logger_, "CUDA/EGL 初始化失败");
        state_.store(DeviceState::Error);
        last_error_.store(ErrorCode::DeviceNotFound);
        return false;
    }

    // ---- 初始化 NvMedia ----
    if (!initNvMedia()) {
        state_.store(DeviceState::Error);
        return false;
    }

    // ---- 打开 I2C 总线 ----
    char i2c_path[64];
    snprintf(i2c_path, sizeof(i2c_path), "/dev/i2c-%d", config_.i2c_bus);
    i2c_fd_ = ::open(i2c_path, O_RDWR);
    if (i2c_fd_ < 0) {
        RCLCPP_WARN(logger_, "无法打开 I2C %s: %s", i2c_path, strerror(errno));
    } else {
        // 设置 IMX678 I2C 地址（0x1A 或 0x36，视模组设计）
        int addr = 0x1A;
        if (ioctl(i2c_fd_, I2C_SLAVE, addr) < 0) {
            RCLCPP_WARN(logger_, "I2C 设置地址 0x%02X 失败", addr);
        } else if (!configSensorIMX678()) {
            RCLCPP_WARN(logger_, "IMX678 配置失败");
        }
    }

    // ---- 初始化 IJP ----
    if (!initIJP()) {
        state_.store(DeviceState::Error);
        return false;
    }

    // ---- 创建缓冲区池 ----
    if (!createBufferPool()) {
        state_.store(DeviceState::Error);
        return false;
    }

    state_.store(DeviceState::Streaming);
    RCLCPP_INFO(logger_, "NvMedia 设备初始化完成");
    return true;
}

bool NvMediaCaptureDevice::startStreaming(FrameCallback callback) {
    if (state_.load() != DeviceState::Streaming) {
        RCLCPP_ERROR(logger_, "设备未就绪");
        return false;
    }

    frame_callback_ = callback;
    streaming_.store(true);

    // 启动采集线程
    capture_thread_ = std::thread(&NvMediaCaptureDevice::captureLoop, this);

    // 启动 IJP 流
    NvMediaStatus status = NvMediaIJPStart(ijp_);
    if (status != NVMEDIA_STATUS_OK) {
        RCLCPP_ERROR(logger_, "IJP Start 失败: %d", status);
        streaming_.store(false);
        return false;
    }

    RCLCPP_INFO(logger_, "IJP 流已启动");
    return true;
}

void NvMediaCaptureDevice::captureLoop() {
    NvMediaImage* current_image = nullptr;
    NvMedia_bufTag tag{};

    while (streaming_.load()) {
        // ---- 从 IJP 获取处理完成的图像 ----
        NvMediaStatus status = NvMediaIJPGetNextOutput(
            ijp_,
            &current_image,
            1000,  // timeout_ms
            &tag
        );

        if (status != NVMEDIA_STATUS_OK || !current_image) {
            if (status == NVMEDIA_STATUS_TIMED_OUT) {
                continue;  // 超时，继续等待
            }
            RCLCPP_WARN(logger_, "IJP GetNextOutput 失败: %d", status);
            break;
        }

        // ---- 获取 CUDA EGL 内存句柄 ----
        // NvMedia Surface → CUDA EGL Frame → GPU 零拷贝内存
        NvMediaSurfaceAccessRead访问 = NvMediaSurfaceAccess_Read;

        // 获取 EGL Frame（用于 CUDA 直接访问）
        CUgraphicsResource cuda_resource;
        CUeglFrame egl_frame;

        status = NvMediaImageLockSurface(
            current_image,
            NvMediaSurfaceAccess_Read,
            &egl_frame
        );

        if (status != NVMEDIA_STATUS_OK) {
            RCLCPP_WARN(logger_, "Surface Lock 失败: %d", status);
            NvMediaIJPOutputRelease(ijp_);
            continue;
        }

        // egl_frame 现在可以直接在 CUDA 中使用（零拷贝！）

        // ---- 构建帧数据 ----
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            uint32_t idx = write_idx_.load() % kQueueSize;
            frame_queue_[idx].frame.data = nullptr;  // GPU 内存，CUDA直接访问
            frame_queue_[idx].frame.width = config_.csi2.width;
            frame_queue_[idx].frame.height = config_.csi2.height;
            frame_queue_[idx].frame.format = PixelFormat::RAW12;  // 原始已处理为RGBA
            frame_queue_[idx].frame.timestamp_ns = tag.timestamp * 1000ULL;  // ms → ns
            frame_queue_[idx].frame.stride = egl_frame.pitch;
            frame_queue_[idx].valid = true;

            write_idx_.store(idx + 1);
        }

        pthread_cond_signal(&queue_cond_);

        // ---- 触发回调 ----
        if (frame_callback_) {
            // 注意：由于 GPU 内存特殊性，回调中必须使用 CUDA 操作
            // 或者通过 cudaMemcpy 同步到 CPU 内存
            FrameBuffer fb;
            fb.width = config_.csi2.width;
            fb.height = config_.csi2.height;
            fb.timestamp_ns = tag.timestamp * 1000ULL;
            fb.format = PixelFormat::RAW12;

            // 提供 EGL frame 给回调（CUDA 零拷贝）
            // 回调方需自己处理 GPU 内存访问
            frame_callback_(fb, fb, {});  // 简化：单目模式
        }

        // ---- 释放图像回 IJP ----
        NvMediaImageUnlockSurface(current_image);
        NvMediaIJPOutputRelease(ijp_);
    }

    RCLCPP_INFO(logger_, "采集线程结束");
}

void NvMediaCaptureDevice::stopStreaming() {
    if (!streaming_.load()) return;

    streaming_.store(false);

    if (ijp_) {
        NvMediaIJPStop(ijp_);
    }

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    RCLCPP_INFO(logger_, "流已停止");
}

void NvMediaCaptureDevice::close() {
    stopStreaming();

    if (buf_pool_) {
        NvMediaBufPoolDestroy(buf_pool_);
        buf_pool_ = nullptr;
    }

    if (ijp_) {
        NvMediaIJPObjectsDestroy(ijp_);
        ijp_ = nullptr;
    }

    if (device_) {
        NvMediaDeviceDestroy(device_);
        device_ = nullptr;
    }

    if (i2c_fd_ >= 0) {
        ::close(i2c_fd_);
        i2c_fd_ = -1;
    }

    state_.store(DeviceState::Closed);
    RCLCPP_INFO(logger_, "设备已关闭");
}

bool NvMediaCaptureDevice::setExposure(uint32_t us) {
    // 曝光时间 → 行数转换
    // IMX678: COARSE_INTEG_TIME = exposure_us / (1/30fps * 1/1080) / 1行时间
    uint32_t lines = (us * 30 * 2160) / 1000000;
    lines = std::min(std::max(lines, 1u), 2160u);

    if (writeI2C(0x0203, static_cast<uint8_t>(lines & 0xFF)) &&
        writeI2C(0x0202, static_cast<uint8_t>((lines >> 8) & 0xFF))) {
        RCLCPP_INFO(logger_, "曝光设置为 %u us (%u lines)", us, lines);
        return true;
    }
    return false;
}

bool NvMediaCaptureDevice::setAnalogGain(float db) {
    // 模拟增益 → 寄存器值
    // IMX678 增益表: 0dB=0x01, 6dB=0x04, 12dB=0x10
    uint8_t gain_reg;
    if (db <= 0) {
        gain_reg = 0x01;  // 0dB
    } else if (db <= 6) {
        gain_reg = 0x04;  // 6dB
    } else if (db <= 12) {
        gain_reg = 0x10;  // 12dB
    } else {
        gain_reg = 0x1C;  // 18dB (max)
    }

    if (writeI2C(0x0204, gain_reg)) {
        RCLCPP_INFO(logger_, "模拟增益设置为 %.1f dB (reg=0x%02X)", db, gain_reg);
        return true;
    }
    return false;
}

bool NvMediaCaptureDevice::setHdrMode(int mode) {
    // IMX678 HDR 模式寄存器
    switch (mode) {
        case 0:  // Linear
            writeI2C(0x0201, 0x00);
            break;
        case 1:  // HDR-X2
            writeI2C(0x0201, 0x03);
            break;
        case 2:  // HDR-X4
            writeI2C(0x0201, 0x07);
            break;
        default:
            return false;
    }
    RCLCPP_INFO(logger_, "HDR 模式设置为 %d", mode);
    return true;
}

}  // namespace
