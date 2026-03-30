# Embodied Vision — 开发者入门指南

## 环境要求

- **ROS2** Humble 或 Jazzy
- **OpenCV4** (≥4.5，带 `opencv_contrib` 用于 ximgproc)
- **CUDA** (可选，用于 GPU 加速)
- **ONNXRuntime** (可选，用于置信度 NN 推理)
- **GTest** (可选，用于单元测试)

## 快速开始

### 1. 构建

```bash
# 克隆到 ROS2 工作空间
cd ~/embodied_vision_ws/src
git clone https://github.com/Terrytan008/embodied_vision.git

# 构建（含 V4L2 + ONNXRuntime，不含 NvMedia）
cd ~/embodied_vision_ws
colcon build --merge-install \
  -DWITH_V4L2=ON \
  -DWITH_ONNXRUNTIME=ON

source install/setup.bash
```

### 2. 模拟器模式（无硬件）

```bash
# 方式 A：launch 参数
ros2 launch stereo_vision_driver driver.launch.py simulator:=true

# 方式 B：环境变量
STEREO_SIMULATOR=1 ros2 launch stereo_vision_driver driver.launch.py
```

### 3. 真实硬件模式

```bash
# 检查设备
ls /dev/video*        # 确认 video0, video2 存在
v4l2-ctl --list-devs  # 查看设备详情

# 启动驱动
ros2 launch stereo_vision_driver driver.launch.py \
  publish_hz:=30 \
  confidence_threshold:=0.65 \
  hdr_mode:=hdr_x2
```

### 4. 验证输出

```bash
# 订阅深度图
ros2 topic echo /stereo_camera_node/depth --once

# 订阅置信度图
ros2 topic echo /stereo_camera_node/confidence --once

# 查看设备状态
ros2 topic echo /stereo_camera_node/status

# 触发在线标定
ros2 service call /stereo_camera_node/recalibrate std_srvs/srv/Trigger
```

## 核心 API 示例

```cpp
#include <stereo_vision/stereo_camera.hpp>

// 创建默认配置
stereo_vision::CameraConfig config;
config.publish_hz = 10;
config.confidence_threshold = 0.65f;
config.hdr_mode = stereo_vision::CameraConfig::HdrMode::X2;

// 创建设备
auto cam = stereo_vision::StereoCamera::create(config);
if (!cam) { /* 处理错误 */ }

// 获取一帧（阻塞，最多 100ms）
auto frame = cam->grabFrame(100);
if (!frame) { /* 超时处理 */ }

// 使用置信度掩码过滤深度
auto mask = stereo_vision::confidenceMask(
    frame->confidence, stereo_vision::ConfidenceLevel::HIGH);
cv::bitwise_and(frame->depth_m, mask, safe_depth);

// IMU 数据
if (frame->imu.has_value()) {
    auto& imu = frame->imu.value();
    printf("Angular vel: %.3f %.3f %.3f rad/s\n",
           imu.gyro[0], imu.gyro[1], imu.gyro[2]);
}
```

## 置信度等级参考

| 等级 | 阈值 | 适用场景 |
|------|------|---------|
| 极高 (VERY_HIGH) | ≥0.85 | 精密操作（抓取）|
| 高 (HIGH) | ≥0.65 | 导航避障 |
| 中 (MEDIUM) | ≥0.40 | 语义感知 |
| 低 (LOW) | >0 | VIO 融合（降权）|
| 无效 (INVALID) | =0 | 传感器失效 |

## 常见问题

### Q: 模拟器模式下 depth 全是 0？
A: 正常，模拟器输出固定的假深度数据。用于验证 ROS2 接口，不用于算法测试。

### Q: 置信度全为 0？
检查：
1. 左右目图像是否正常（检查 `left_rect` 是否非空）
2. 标定参数是否有效（baseline_mm 是否 > 0）
3. SGBM num_disparities 是否足够（场景最大深度 / (fx*B)）

### Q: 如何调整 SGBM 参数？
修改 `StereoCamera::create()` 调用处的 `SGBMDepthEngine::Params`：
```cpp
sgbm_params.num_disparities = 256;  // 更大视差范围 → 更远深度
sgbm_params.block_size = 9;         // 更大窗口 → 更平滑但更慢
sgbm_params.min_confidence = 0.5f;  // 更低阈值 → 更多有效点
```

### Q: ONNX 模型路径在哪配置？
launch 文件参数 `onnx_model_path`，或在代码中：
```cpp
config.onnx_model_path = "/path/to/confidence_model.onnx";
```
空字符串 = 不使用 NN 推理，使用传统置信度。
