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
| **SGBM深度计算** | `stereo_depth.cpp` | Semi-Global Matching + 置信度估计 |
| **置信度NN模型** | `train_confidence.py` | 轻量CNN训练 pipeline |
| **ONNX导出** | `export_onnx.py` | PyTorch→ONNX→TensorRT |
| **C++推理** | `confidence_onnx.cpp` | ONNXRuntime推理引擎 |
| **V4L2捕获** | `nv_csi_capture.cpp` | CSI-2 视频采集 |
| **BMI088 IMU** | `imu_bmi088.cpp` | IMU驱动 |

## 快速开始

### 构建

```bash
# ROS2 工作空间
cd ~/embodied_vision_ws
colcon build --merge-install
source install/setup.bash
```

### 模拟器模式（无硬件）

```bash
ros2 launch stereo_vision_driver driver.launch.py simulator:=true
```

### 真实硬件模式

```bash
ros2 launch stereo_vision_driver driver.launch.py publish_hz:=10
```

### 订阅话题

```bash
# 深度图 (米)
ros2 topic echo /stereo_camera_node/depth

# 置信度图 (0.0~1.0)
ros2 topic echo /stereo_camera_node/confidence

# 左目图像
ros2 topic echo /stereo_camera_node/left/image_rect

# IMU
ros2 topic echo /stereo_camera_node/imu
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
