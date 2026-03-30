// Embodied Vision — 硬件平台自动选择工厂
// hardware_selector.cpp
//
// 自动检测当前平台，返回最适合的 Capture 实现：
//   NVIDIA DRIVE/Jetson (Linux tegra) → NvMediaCaptureDevice
//   其他 Linux / 模拟器               → V4L2CaptureDevice

#include "stereo_vision/hardware/capture_base.hpp"
#include "stereo_vision/hardware/v4l2_capture.hpp"
#include <rclcpp/rclcpp.hpp>

#ifdef ENABLE_NVMEDIA
#include "stereo_vision/hardware/nv_nvmedia_capture.hpp"
#endif

#include <fstream>
#include <cstring>

namespace {

// 检测是否为 NVIDIA Jetson/DRIVE 平台
bool isNvidiaTegraPlatform() {
    // 检查 /proc/device-tree/compatible
    std::ifstream f("/proc/device-tree/compatible");
    if (!f.is_open()) return false;

    char buf[512];
    f.read(buf, sizeof(buf) - 1);
    f.close();
    buf[511] = '\0';

    const char* nvidia_markers[] = {
        "nvidia,tegra",
        "nvidia,phantom",
        "p3450",       // Jetson Nano
        "p2822",       // Jetson AGX Xavier
        "p3701",       // Jetson AGX Orin
        "p3767",       // Jetson NX Orin
    };

    for (const auto* marker : nvidia_markers) {
        if (std::strstr(buf, marker)) {
            return true;
        }
    }
    return false;
}

}  // anonymous namespace

namespace stereo_vision {
namespace hardware {

/**
 * @brief 创建平台最优 Capture 设备
 *
 * @param config 设备配置
 * @param preferred_backend 强制指定后端 ("v4l2" / "nvmedia" / "auto")
 */
std::unique_ptr<CaptureDevice>
createCaptureDevice(const DeviceConfig& config,
                   const std::string& preferred_backend = "auto") {
    std::string backend = preferred_backend;

    if (backend == "auto") {
        if (isNvidiaTegraPlatform()) {
            backend = "nvmedia";
        } else {
            backend = "v4l2";
        }
    }

    if (backend == "nvmedia") {
#ifdef ENABLE_NVMEDIA
        // 编译时有 NvMedia 支持
        return std::make_unique<NvMediaCaptureDevice>();
#else
        // 编译时无 NvMedia，回退到 V4L2
        RCLCPP_WARN(rclcpp::get_logger("HardwareFactory"),
                   "NvMedia 编译支持未启用，回退到 V4L2");
        return std::make_unique<V4L2CaptureDevice>();
#endif
    }

    // 默认 V4L2
    return std::make_unique<V4L2CaptureDevice>();
}

/**
 * @brief 获取平台信息（用于日志和诊断）
 */
std::string getPlatformInfo() {
    std::string info;

    if (isNvidiaTegraPlatform()) {
        info = "NVIDIA Platform (DRIVE/Jetson) - NvMedia 可用";
    } else {
        info = "Generic Linux - 使用 V4L2";
    }

    // 附加 CPU/GPU 信息
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        char buf[256];
        cpuinfo.read(buf, sizeof(buf) - 1);
        buf[255] = '\0';
        // 查找 model name
        const char* model = std::strstr(buf, "model name");
        if (model) {
            info += " | ";
            const char* start = std::strchr(model, ':');
            if (start) {
                start += 2;
                const char* end = std::strchr(start, '\n');
                if (end) {
                    info.append(start, end - start);
                }
            }
        }
        cpuinfo.close();
    }

    return info;
}

}  // namespace hardware
}  // namespace stereo_vision
