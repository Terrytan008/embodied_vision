# Embodied Vision — 双目视觉模组

> 具身机器人的确定性之眼

**定位**：面向 NVIDIA DRIVE AGX Thor 平台的高性能双目视觉模组，专注于透明物体、逆光、运动畸变等具身机器人典型难点场景。

**核心特性**：
- 在线自标定（Thor平台<1秒收敛）
- 帧级别置信度热力图
- IMU紧耦合运动畸变校正
- 开放 ROS2 Humble/Jazzy 驱动源码

## 项目结构

```
embodied_vision/
├── src/
│   ├── stereo_vision_driver/     # ROS2 驱动包
│   └── stereo_vision_calibration/ # 标定工具
├── hardware/                      # 硬件设计（待补充）
├── docs/                         # 文档
└── scripts/                      # 构建脚本
```

## 快速开始

### 依赖

- ROS2 Humble 或 Jazzy
- NVIDIA DRIVE AGX Thor (或 Jetson Orin 作为开发替代)
- CUDA 12+
- Python 3.10+

### 构建

```bash
cd src/stereo_vision_driver
colcon build --merge-install
source install/setup.bash

# 运行驱动
ros2 launch stereo_vision_driver driver.launch.py
```

## 核心API

```cpp
#include <stereo_vision/stereo_camera.hpp>

auto cam = StereoCamera::create(config, node);
auto frame = cam->grabFrame(100);  // 100ms 超时

// 帧数据包含：
//   - depth_m         深度图（米）
//   - confidence      置信度图（0.0~1.0）
//   - disparity       视差图（像素）
//   - metadata        元数据（运动状态/温度/标定状态）
```

## 文档

- [API参考](docs/api_reference/)
- [开发者指南](docs/developer_guide/)
- [硬件规格](docs/hardware/)

## 社区

- GitHub Issues：Bug报告和功能请求
- ROS2 生态项目欢迎提交 PR

## 开源许可

- 驱动和算法：MIT License
- 硬件设计：待定

---

*本模组专为具身机器人设计，以"决策可信度"而非"物理精度"为核心设计哲学。*
