# Embodied Vision — 系统架构

## 整体架构

```
┌─────────────────────────────────────────────────────┐
│           StereoCameraNode (ROS2 组件节点)           │
│                                                     │
│   timer → grabFrame() → ImageTransport → ROS2 topic│
└──────────────────────┬──────────────────────────────┘
                       │ StereoCamera::grabFrame()
┌──────────────────────▼──────────────────────────────┐
│              StereoCamera (Impl pImpl)              │
│                                                     │
│  ┌──────────────┐  GrabFrame Thread ─────────────────┤
│  │ left_raw_    │ ◄─── cv::Mat                      │
│  │ right_raw_   │ ◄─── cv::Mat  ←── capture callbacks│
│  └──────┬───────┘                                    │
│         │ frame_cv_.wait()                           │
│  ┌──────▼──────────────────────────────┐             │
│  │  SGBMDepthHelper                    │             │
│  │    └─ IMU运动补偿 (applyMotionCorrection)        │
│  │    └─ SGBM视差计算 (computeDisparity)            │
│  │    └─ 置信度估计 (computeConfidence)              │
│  │    └─ 视差→深度 (disparityToDepth)               │
│  └──────┬──────────────────────────────┘             │
│         │ FrameData (depth_m, confidence, left_rect) │
└─────────┼───────────────────────────────────────────┘
          │
    ┌─────▼─────┐
    │IMU线程    │  BMI088Driver::read() → IMUData
    └───────────┘

采集层:
  ┌─────────────────┐    ┌─────────────────┐
  │V4L2CaptureDevice│    │NvMediaCaptureDevice
  │ (通用 Linux)    │    │ (Jetson/DRIVE) │
  └────────┬────────┘    └────────┬────────┘
           │                      │
           └─── CaptureDevice ────┘  (统一基类)
```

## 核心类说明

### StereoCamera (stereo_camera.hpp)
**工厂类**，通过 `StereoCamera::create()` 创建 `RealCamera`（pImpl 模式）。
实际工作由内部的 `Impl` 类完成。

### Impl (stereo_camera_impl.cpp)
- 管理两个 `CaptureDevice`（左右目）
- 管理 `BMI088Driver` IMU
- 管理 `SGBMDepthHelper` 深度引擎
- 维护帧缓冲 `left_raw_` / `right_raw_`
- 运行**在线标定器后台线程**

### SGBMDepthHelper (stereo_depth.cpp)
封装 `SGBMDepthEngine`，提供：
- `compute()`: 标准 SGBM 深度计算
- `computeWithIMU()`: 带 IMU 运动补偿的深度计算

**深度计算流程**:
1. **预处理**: Bayer demosaic → 灰度化 → Gamma校正
2. **立体校正**: `cv::stereoRectify()` → `cv::remap()`
3. **SGBM视差**: `cv::StereoSGBM::create()` + 左右一致性检查
4. **亚像素细化**: 抛物线插值
5. **视差→深度**: `Z = fx * B / d`
6. **置信度估计**: Sobel纹理 + 匹配代价方差 + 唯一性 的几何平均
7. **安全掩码**: 无效区域清零

### IMU运动补偿 (applyMotionCorrection)
对每帧图像应用基于 gyro 的逆扭曲：

```
θ = gyro_z * kExposureSec
Δx ≈ -θ * (y - cy)          // 绕Z轴旋转在图像平面的剪切

cv::remap(left_raw, left_comp, map_x, map_y, ...)
```

### 在线标定器 (calibrator_thread_)
后台线程等待 `calibrator_trigger_.load()` 信号，触发后：
1. 收集最近 30 帧高置信度图像对
2. 梯度下降优化 `baseline_scale`
3. Atomically 更新 `impl_->calib_`

## 硬件抽象层

```
CaptureDevice (基类)
├── V4L2CaptureDevice  (标准 Linux V4L2)
└── NvMediaCaptureDevice (NVIDIA Jetson/DRIVE, 需要 DriveOS SDK)
```

平台检测: `hardware_selector.cpp` 检查 `/proc/device-tree/compatible` 是否含 `nvidia,tegra`。

## ROS2 接口

| 话题 | 类型 | 说明 |
|------|------|------|
| `left/image_rect` | sensor_msgs/Image | 左目校正图像 (rgb8/mono8) |
| `right/image_rect` | sensor_msgs/Image | 右目校正图像 (rgb8/mono8) |
| `depth` | sensor_msgs/Image | 深度图 (32FC1, 米) |
| `confidence` | sensor_msgs/Image | 置信度图 (32FC1, 0.0~1.0) |
| `imu` | sensor_msgs/Imu | IMU 原始数据 |

| 服务 | 类型 | 说明 |
|------|------|------|
| `~/recalibrate` | std_srvs/Trigger | 触发在线标定重收敛 |
