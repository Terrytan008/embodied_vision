# Changelog

所有重要变更按提交顺序记录。

## [0.1.0] — 2026-03-30

### 修复 (Bug Fixes)

#### 核心驱动

- **V4L2采集线程未启动** (`stereo_camera_impl.cpp`)
  - `initialize()` 原来只创建设备但从未调用 `startStreaming()`
  - 现已添加 `startStreaming()` 调用及左右目录制回调
  - 采集线程写入 `Impl::left_raw_/right_raw_`，`grabFrame()` 通过 `condition_variable` 等待

- **DeviceConfig 缺少 left_device/right_device** (`camera_types.hpp`)
  - 新增 `left_device="/dev/video0"`、`right_device="/dev/video2"` 字段
  - 修复 v4l2_capture.cpp 引用 `cfg.left_device` 的编译错误

- **createCaptureDevice 声明与实现不匹配** (`capture_base.hpp`)
  - 声明从 `(const std::string&)` 修正为 `(const DeviceConfig&, const std::string&)`
  - 与 `hardware_selector.cpp` 实现一致

- **图像编码硬编码 rgb8** (`stereo_camera_node.cpp`)
  - 灰度图（debayer 后单通道）发布 `mono8`，彩图发布 `rgb8`
  - 而非之前硬编码 `rgb8`

- **Bayer demosaic 缺失** (`stereo_camera_impl.cpp`)
  - 新增 `parseRaw12()`：正确解析 Sony IMX678 RAW12 格式（每2像素3字节）
  - 新增 `debayerRGGG()`：使用 OpenCV `COLOR_BAYERBG2BGR` 双线性插值
  - 替代原来只截断高4bit的错误做法

- **IMU运动补偿占位空壳** (`stereo_depth.cpp`)
  - 实现 `applyMotionCorrection()`：gyro → 曝光时间内角度增量 → `cv::remap` 逆扭曲
  - 仅在旋转超过阈值（~0.1度）时生效

- **HDR V4L2 直接返回 false** (`v4l2_capture.cpp`)
  - 实现 `setHdrMode()`：支持 `V4L2_CID_MANELEXPO_MODE`（厂商扩展）
  - 降级：软件曝光交替+融合（驱动不支持时）

- **ConfidenceInference ODR违规** (`confidence_onnx.hpp/cpp`)
  - 原 `.hpp` 声明与 `.cpp` 定义的 `ConfidenceInference` private 成员不一致
  - 合并为单一完整类定义，正确分离接口与实现

#### 在线标定

- **在线标定只有日志无实现** (`stereo_camera_impl.cpp`)
  - 新增后台 `calibrator_thread_`：收集高置信帧，梯度下降优化 `baseline_scale`
  - 触发信号 `calibrator_trigger_.store(true)`，`getCalibration()` 返回实时 `impl_->calib_`

- **标定 Service 响应无信息** (`stereo_camera_node.cpp`)
  - `~/recalibrate` 响应消息现在包含 `baseline`、`confidence`
  - 新增 `~/get_calibration` 查询服务

#### 包结构

- **stereo_vision_msgs 包缺失**（新增包）
  - 从 `stereo_vision_driver/msg/` 独立为独立 ROS2 interface 包
  - 解决 `stereo_vision_driver` 依赖 `stereo_vision_msgs` 但不存在的编译错误

- **hardware_factory.cpp 死代码**（已删除）
  - 未编译入库但包含与实际工厂函数同名的 `createCaptureDevice`
  - 已删除，消除 ODR 阴影

- **CMakeLists.txt rosidl 冲突** (`stereo_vision_driver/CMakeLists.txt`)
  - 移除本地 `rosidl_generate_interfaces`（已迁移到 `stereo_vision_msgs`）
  - 新增 `find_package(stereo_vision_msgs REQUIRED)`
  - 修复测试可执行文件链接方式（链接库而非编译源文件）

#### Launch / 构建

- **Launch simulator 参数逻辑错误** (`driver.launch.py`)
  - 原来 `publish_hz` 的三元表达式写法不正确
  - 重写为 `OpaqueFunction`，launch 时动态计算 `hz=0 if simulator=true`

- **ONNX 模型路径硬编码** (`stereo_camera.hpp/node.cpp/impl.cpp`)
  - 新增 `CameraConfig::onnx_model_path`
  - `StereoCameraNode` 读取参数，`Impl` 初始化 `ConfidenceInference`

- **build.sh colcon 用法错误** (`scripts/build.sh`)
  - 移除错误的 `--dry-run` 和 cmake clean 逻辑
  - 修正为正确的增量构建命令

### 新增 (New Features)

- **GTest 单元测试** (`test/test_stereo_depth.cpp`)
  - 7个用例：SGBM输出尺寸、置信度范围、深度物理范围、IMU运动补偿、视差公式、置信度工具函数

- **开发者文档** (`docs/developer_guide/`)
  - `getting_started.md`：环境要求、构建、模拟器、ROS2接口
  - `architecture.md`：系统架构图、核心类说明、数据流
  - `calibration.md`：工厂标定+在线标定+质量验证

- **CI/CD 增强** (`.github/workflows/ci.yml`)
  - 安装 `libgtest-dev`
  - `colcon test` 运行 GTest 单元测试
  - Python 脚本 `python3 --help` 健康检查
  - flake8 linting

- **工作空间初始化脚本** (`scripts/setup_workspace.sh`)
  - 自动检测并 source ROS2 环境
  - `ev-build`、`ev-sim`、`ev-status` 便捷别名

- **stereo_vision_calibration 包结构** (`package.xml` + `setup.py`)
  - 补全 ROS2 Python 包元数据

### 文档 (Documentation)

- **README.md** 全面更新：环境要求、构建选项、服务调用、模块映射

---

## [0.1.0] — 2026-03-30（下午）

### 修复 (Afternoon session)

- **OpenCV 头文件路径错误** (`stereo_camera.hpp`)
  - 修复：`opencv2/opencv2.hpp` → `opencv2/opencv.hpp`
  - 影响：所有编译单元均受影响，是之前遗留的阻塞性 bug

- **hardware_selector.cpp 缺失 rclcpp 头文件**
  - 修复：添加 `#include <rclcpp/rclcpp.hpp>`（`RCLCPP_WARN` 使用所需）

- **ConfidenceInference 接入 realGrabFrame pipeline**
  - SGBM 置信度计算后，若 `confidence_nn_` 已加载：
    - 左右图转灰度 → `confidence_nn_->infer(left_gray, right_gray, disparity)`
    - 融合：NN×0.6 + SGBM×0.4
  - ONNX 模型加载在 `Impl::initialize()` 中，pipeline 接入在 `realGrabFrame()`

- **online_calibrator.py Jacobian bug**
  - 旧版：rotation Jacobian 全零（数值微分求导写错）
  - 新版：6 次前向投影数值微分，Rodrigues 旋转矩阵正确实现
  - numba `@jit(nopython=True, cache=True)` 加速

- **ConfidenceInference ODR 违规**
  - `.hpp` 声明与 `.cpp` 定义 private 成员不一致
  - 合并为单一完整类定义（`.hpp`），`.cpp` 仅保留 out-of-line 方法

- **stereo_vision_msgs 包独立**
  - 从 `stereo_vision_driver/msg/` 独立为独立 ROS2 interface 包
  - 解决 `stereo_vision_driver` 依赖 `stereo_vision_msgs` 但不存在的编译错误

- **hardware_factory.cpp 死代码删除**
  - 未编译入库但包含同名 `createCaptureDevice` 阴影

### 新增

- `.clang-format`：代码风格配置（Google style，120 列）
- `scripts/setup_workspace.sh`：ROS2 环境 + 工作空间初始化 + 便捷别名
