# Embodied Vision — 双目视觉模组

> 具身机器人的确定性之眼

**定位**：面向具身机器人的高性能双目视觉模组，专注于透明物体、逆光、运动畸变等典型难点场景。

**核心特性**：
- 在线自标定（Thor平台GPU加速，<1秒收敛）
- 帧级置信度热力图（0.0~1.0，下游决策友好）
- IMU紧耦合运动畸变校正
- 开放 ROS2 Humble/Jazzy 驱动源码（MIT License）

## 硬件规格

| 参数 | 值 |
|------|-----|
| Sensor | Sony IMX678 × 2 (4K/30fps) |
| 基线 | 80mm |
| IMU | Bosch BMI088 (6轴，车规) |
| 接口 | FPD-Link III → CSI-2 |
| 深度范围 | 0.1m ~ 10m |
| 目标BOM成本 | ¥680 (1K量) |

## 核心模块

| 模块 | 文件 | 说明 |
|------|------|------|
| **SGBM深度计算** | `src/stereo_vision_driver/src/stereo_depth.cpp` | Semi-Global Matching + 置信度估计 |
| **置信度NN模型** | `src/stereo_vision_calibration/scripts/train_confidence.py` | 轻量CNN训练 pipeline |
| **ONNX导出** | `src/stereo_vision_calibration/scripts/export_onnx.py` | PyTorch→ONNX |
| **C++推理** | `src/stereo_vision_driver/src/hardware/confidence_onnx.cpp` | ONNXRuntime推理引擎 |
| **V4L2捕获** | `src/stereo_vision_driver/src/hardware/v4l2_capture.cpp` | Linux V4L2 CSI-2 采集 |
| **NvMedia捕获** | `src/stereo_vision_driver/src/hardware/nv_nvmedia_capture.cpp` | NVIDIA Jetson/DRIVE (需DriveOS SDK) |
| **BMI088 IMU** | `src/stereo_vision_driver/src/hardware/imu_bmi088.cpp` | IMU驱动 (200Hz I2C) |
| **消息类型** | `src/stereo_vision_msgs/msg/` | StereoCalibrationInfo, StereoDeviceStatus |

## 快速开始

### 环境要求
- **ROS2** Humble 或 Jazzy
- **OpenCV4** (≥4.5)
- **GTest** (可选，单元测试)
- **ONNXRuntime** (可选，NN置信度推理)

### 构建

```bash
cd ~/embodied_vision_ws/src/embodied_vision

# 完整构建（含ONNX支持）
colcon build --merge-install \
  --cmake-args -DENABLE_ONNXRUNTIME=ON

source install/setup.bash
```

### 模拟器模式（无硬件）

```bash
ros2 launch stereo_vision_driver driver.launch.py simulator:=true
```

### 真实硬件模式

```bash
ros2 launch stereo_vision_driver driver.launch.py \
  publish_hz:=30 \
  confidence_threshold:=0.65 \
  hdr_mode:=hdr_x2
```

### 订阅话题

```bash
# 深度图 (米, 32FC1)
ros2 topic echo /stereo_camera_node/depth

# 置信度图 (0.0~1.0, 32FC1)
ros2 topic echo /stereo_camera_node/confidence

# 左目校正图像
ros2 topic echo /stereo_camera_node/left/image_rect

# IMU
ros2 topic echo /stereo_camera_node/imu
```

### 服务

```bash
# 触发在线标定
ros2 service call /stereo_camera_node/recalibrate std_srvs/srv/Trigger

# 查询当前标定参数
ros2 service call /stereo_camera_node/get_calibration std_srvs/srv/Trigger
```

## 核心API

```cpp
#include <stereo_vision/stereo_camera.hpp>

// 创建设备（默认配置即可）
auto cam = StereoCamera::create();

// 获取一帧（含所有数据）
auto frame = cam->grabFrame(100);  // 100ms超时

// 使用置信度掩码
auto mask = confidenceMask(frame->confidence, ConfidenceLevel::HIGH);
auto safe_depth = cv::Mat();
cv::bitwise_and(frame->depth_m, mask, safe_depth);

// 触发在线标定
cam->triggerRecalibration();
```

## 置信度等级

| 等级 | 阈值 | 适用场景 |
|------|------|---------|
| 极高 | ≥0.85 | 精密操作（抓取）|
| 高 | ≥0.65 | 导航避障 |
| 中 | ≥0.40 | 语义感知 |
| 低 | >0 | VIO融合（降权）|
| 无效 | =0 | 传感器失效 |

## 文档

- [开发者指南](docs/developer_guide/)
- [API参考](docs/api_reference/)
- [硬件规格](docs/hardware/)

## 构建状态

![CI](https://github.com/Terrytan008/embodied_vision/actions/workflows/ci.yml/badge.svg)

## 开源许可

- 驱动和算法：MIT License
- 硬件设计：待定

---

*以"决策可信度"而非"物理精度"为核心设计哲学*
