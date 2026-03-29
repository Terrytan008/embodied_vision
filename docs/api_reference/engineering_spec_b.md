# 工程规格推导 B：软件 API 设计规范

> 面向具身机器人的确定性双目视觉 API——以"决策可信度"为核心设计哲学

---

## 1. 设计原则

### 1.1 核心哲学

```
决策可信度 > 物理精度

用户需要的不只是"深度值"，
而是一个"能放心用于下游决策的深度值"。
```

### 1.2 API 设计四原则

| 原则 | 说明 | 违反示例 |
|------|------|---------|
| **零隐式假设** | 所有行为由参数控制，无默认值猜谜 | `depth.getPixel(x,y)` 失败时抛异常还是返回0？|
| **置信度第一** | 深度值永远附带置信度，无置信度的深度 = 不可信 | `getDepth()` 返回 cv::Mat，但用户不知道哪些像素可信 |
| **错误可诊断** | 错误代码有唯一含义，有日志追踪 | `ErrorCode::HardwareError` —— 哪里的硬件？|
| **线程安全默认** | 多线程 ROS2 环境下安全 | 多线程同时调用 `setExposure()` 产生竞态 |

### 1.3 参数命名规范

```
命名格式：snake_case（与 ROS2 一致）

传感器控制参数：
  {noun}_{property}
  - exposure_us          (微秒曝光)
  - analog_gain_db       (分贝增益)
  - hdr_mode             (HDR模式: 0=linear, 1=hdr-x2, 2=hdr-x4)

算法控制参数：
  {algo}_{property}
  - sgbm_num_disparities
  - sgbm_block_size
  - conf_threshold_high   (高精度阈值)
  - conf_threshold_nav    (导航阈值)

输出话题参数：
  {topic}_{property}
  - depth_unit (Unit::METERS | Unit::MILLIMETERS)
  - confidence_mode (raw | thresholded | weighted)
```

---

## 2. 核心接口

### 2.1 C++ 主类：StereoCamera

```cpp
namespace stereo_vision_driver {

/**
 * @brief 双目视觉模组主接口
 *
 * 使用工厂创建：
 *   auto cam = StereoCamera::create(config);
 *
 * 典型用法：
 *   auto frame = cam->grabFrame(100ms);
 *   if (frame && frame->confidence) {
 *       auto safe = applyConfidenceMask(frame->depth, frame->confidence,
 *                                       ConfidenceLevel::HIGH);
 *       publish(safe);
 *   }
 */
class StereoCamera {
public:
    using SharedPtr = std::shared_ptr<StereoCamera>;

    // ========== 工厂 ==========

    /**
     * @brief 创建双目相机实例
     * @param config 相机配置（含命名空间，用于 ROS2 参数服务器）
     * @return 共享指针，失败返回 nullptr
     */
    static SharedPtr create(const Config& config);
    static SharedPtr create(const std::string& ros2_namespace = "stereo_camera");

    // ========== 生命周期 ==========

    /// 析构：自动停止流、释放设备
    ~StereoCamera();

    /// 启动视频流（启动后台采集线程）
    bool startStream();

    /// 停止视频流
    void stopStream();

    /// 获取当前状态
    DeviceState getState() const;

    // ========== 帧采集 ==========

    /**
     * @brief 采集一帧（含深度 + 置信度 + IMU）
     * @param timeout_ms 超时毫秒
     * @return 帧数据指针（永远非空，内部用 validity 标记无效帧）
     * @note 返回的 frame 引用计数 +1，调用者用完自动释放
     */
    StereoFrame::SharedPtr grabFrame(uint32_t timeout_ms = 100);

    // ========== 同步/异步采集 ==========

    /**
     * @brief 注册帧回调（推送模式）
     * @param callback 回调函数，接收完整帧
     * @note 回调在后台线程调用，注意线程安全
     */
    void registerCallback(FrameCallback callback);

    // ========== 标定 ==========

    /**
     * @brief 触发在线重标定
     * @param mode 标定模式（快速/完整）
     * @return true 标定已触发（异步完成，结果通过回调通知）
     */
    bool triggerRecalibration(CalibrationMode mode = CalibrationMode::Fast);

    /// 获取当前标定参数
    CalibrationParams getCalibration() const;

    /// 加载标定文件
    bool loadCalibration(const std::string& yaml_path);

    /// 保存标定到文件
    bool saveCalibration(const std::string& yaml_path) const;

    // ========== 传感器控制 ==========

    /// 设置曝光时间（微秒）
    bool setExposure(uint32_t us);

    /// 获取当前曝光时间
    uint32_t getExposure() const;

    /// 设置模拟增益（分贝）
    bool setAnalogGain(float db);

    /// 获取当前模拟增益
    float getAnalogGain() const;

    /// 设置 HDR 模式
    bool setHdrMode(int mode);  // 0=linear, 1=hdr-x2, 2=hdr-x4

    // ========== 诊断 ==========

    /// 获取传感器状态（温度/电压/帧率统计）
    DeviceStatus getStatus() const;

    /// 获取版本信息
    std::string getVersion() const;

    // ========== 不可复制 ==========
    StereoCamera(const StereoCamera&) = delete;
    StereoCamera& operator=(const StereoCamera&) = delete;

private:
    StereoCamera();  // 私有构造函数（用 create 工厂）
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace stereo_vision_driver
```

### 2.2 帧数据：StereoFrame

```cpp
namespace stereo_vision_driver {

/**
 * @brief 单帧完整数据
 *
 * 设计原则：
 * - 所有矩阵同分辨率、同时间戳
 * - 无效像素 value=0，confidence=0.0
 * - 内存由 FramePool 管理，避免频繁 malloc
 */
struct StereoFrame : public std::enable_shared_from_this<StereoFrame> {

    // ---- 时空信息 ----
    uint64_t timestamp_ns = 0;        // 采集时刻（Unix时间戳，纳秒）
    uint32_t sequence = 0;            // 帧序号（检测丢帧）

    // ---- 图像数据 ----
    cv::Mat left_image;               // 左目矫正后灰度/彩色图
    cv::Mat right_image;              // 右目矫正后灰度图
    ImageFormat left_format = ImageFormat::GRAY8;

    // ---- 深度与置信度 ----
    cv::Mat depth_m;                  // 深度图（米，CV_32FC1，0=无效）
    cv::Mat confidence;              // 置信度图（0.0~1.0，CV_32FC1）

    // ---- IMU 数据（紧耦合）----
    std::vector<ImuSample> imu_samples;  // 采集时刻前后的 IMU 样本
    // 注：IMU 采样率 200Hz，30fps 相机 ≈ 每帧 ~6 个 IMU 样本

    // ---- 标定信息 ----
    CalibrationParams calib;

    // ---- 元数据 ----
    FrameMetadata metadata;  // sensor_temp / exposure_us / gain_db / ...

    // ---- 辅助方法 ----

    /// 检查像素是否有效（深度 > 0 且置信度足够）
    bool isValid(int x, int y, float min_confidence = 0.65f) const;

    /// 获取有效像素掩码
    cv::Mat getValidMask(float min_confidence = 0.65f) const;

    /// 置信度等级掩码
    cv::Mat getConfidenceMask(ConfidenceLevel level) const;

    /// 应用置信度掩码到深度图
    cv::Mat applyConfidenceMask(const cv::Mat& depth,
                               float min_confidence) const;

    /// 取深度值（带有效性检查）
    std::optional<float> getDepthAt(int x, int y, float min_confidence = 0.65f) const;

    /// 转换单位
    cv::Mat depthInMillimeters() const;  // 米→毫米
    cv::Mat depthInMeters() const;      // 确保米（内部已是米）

    /// 释放缓存（帮助 FramePool 回收）
    void release();

private:
    friend class FramePool;
    int ref_count_ = 0;  // 引用计数（智能指针用）
};

/**
 * @brief 置信度等级
 */
enum class ConfidenceLevel {
    NONE      = 0,  // 置信度 = 0（完全无效）
    LOW       = 1,  // 0.0 < conf < 0.40：VIO 融合（降权使用）
    MEDIUM    = 2,  // 0.40 ≤ conf < 0.65：语义感知
    HIGH      = 3,  // 0.65 ≤ conf < 0.85：导航避障
    VERY_HIGH = 4,  // 0.85 ≤ conf ≤ 1.00：精密操作
};

}  // namespace stereo_vision_driver
```

### 2.3 配置：Config

```cpp
namespace stereo_vision_driver {

/**
 * @brief 相机配置参数
 *
 * 所有参数都有默认值，但默认值不是"最优解"
 * 调用者必须理解每个参数的含义
 */
struct Config {
    // ---- ROS2 标识 ----
    std::string ros_node_name = "stereo_camera_node";
    std::string ros_namespace = "stereo_camera";

    // ---- 发布控制 ----
    float publish_hz = 10.0f;       // 发布频率（0=模拟模式）
    bool publish_depth = true;
    bool publish_confidence = true;
    bool publish_raw_images = false; // 默认关闭，节省带宽

    // ---- 算法参数 ----

    // SGBM 深度计算
    struct {
        int  num_disparities = 128;         // 必须是 16 的倍数
        int  block_size = 7;                // SAD 窗口，必须是奇数
        int  P1 = 0;                        // 自动计算：8 * block_size²
        int  P2 = 0;                        // 自动计算：32 * block_size²
        int  uniqueness_ratio = 15;         // 匹配唯一性惩罚
        int  speckle_window_size = 100;     // 散斑滤波窗口
        int  speckle_range = 32;            // 散斑范围
        float min_confidence = 0.65f;       // 最小置信度阈值
    } sgbm;

    // 置信度计算
    struct {
        std::string model_path;             // ONNX 模型路径（空=使用SGBM内置）
        float onnx_threshold = 0.65f;      // ONNX 置信度阈值
        bool use_nn_fallback = false;       // SGBM置信度低时回退到NN
        bool use_gpu = true;                // 使用 GPU 推理
    } confidence;

    // 在线标定
    struct {
        bool auto_calibrate = false;        // 启动时自动检查/执行标定
        int min_matches = 100;              // 触发标定的最小特征匹配数
        float min_motion = 0.1f;            // 触发标定的最小运动量（米）
        CalibrationMode mode = CalibrationMode::Fast;
    } calibration;

    // ---- 传感器参数 ----
    struct {
        uint32_t exposure_us = 5000;        // 曝光时间（微秒）
        float analog_gain_db = 0.0f;        // 模拟增益（分贝）
        int    hdr_mode = 0;               // 0=linear, 1=hdr-x2, 2=hdr-x4
        int    i2c_bus = 1;                // I2C 总线编号
    } sensor;

    // ---- 硬件配置 ----
    struct {
        std::string left_device = "/dev/video0";
        std::string right_device = "/dev/video1";
        int csi2_port = 0;                 // CSI-2 端口号
        int csi2_lanes = 4;               // Lane 数量
        int width = 3840;
        int height = 2160;
    } hardware;

    // ---- IMU 配置 ----
    struct {
        bool enabled = true;
        int  accel_range = 16;            // g 范围：±16g
        int  gyro_range = 2000;            // °/s 范围：±2000°/s
        int  accel_odr = 400;              // 加速度计 ODR：400Hz
        int  gyro_odr = 200;               // 陀螺仪 ODR：200Hz
    } imu;

    // ---- 帧回调（可选）----
    std::function<void(StereoFrame::SharedPtr)> frame_callback;

    // ---- 诊断 ----
    struct {
        bool publish_diagnostics = true;
        int  stats_window_size = 30;       // 统计窗口（帧数）
    } diagnostics;
};

}  // namespace stereo_vision_driver
```

### 2.4 标定参数：CalibrationParams

```cpp
namespace stereo_vision_driver {

/**
 * @brief 双目标定参数
 *
 * 支持三种精度模式：
 * - FLOAT64：原始标定结果（双精度浮点）
 * - FLOAT32：转换后单精度（减少传输开销）
 * - FIXED16_Q8：定点数量化（嵌入式友好，Q8=小数8位）
 */
struct CalibrationParams {
    // ---- 左目内参 ----
    cv::Matx33f K_left;        // 焦距 + 主点
    cv::Vec4f  D_left;         // 畸变系数 [k1, k2, p1, p2]
    cv::Size   image_size_left;

    // ---- 右目内参 ----
    cv::Matx33f K_right;
    cv::Vec4f  D_right;
    cv::Size   image_size_right;

    // ---- 双目外参 ----
    cv::Vec3f  T;              // 右目相对左目的平移向量（米）
                                // T[0] = B = 基线距离 ≈ 0.080m
    cv::Vec3f  R;              // 右目相对左目的旋转向量（Rodrigues）

    // ---- 立体校正 ----
    cv::Matx33f R_left_rec;   // 左目校正旋转
    cv::Matx33f R_right_rec;  // 右目校正旋转
    cv::Vec3f  P_left;        // 左目投影矩阵 [fx, 0, cx, 0]
    cv::Vec3f  P_right;       // 右目投影矩阵 [fx, 0, cx, fx*baseline]
    cv::Size   image_size_rec; // 校正后图像尺寸

    // ---- 在线标定专用 ----
    struct {
        uint64_t last_update_ns = 0;
        int      update_count = 0;
        float    rms_error = 0.0f;     // 重投影均方根误差（像素）
        bool     is_valid = false;
    } online_calib_meta;

    // ---- 方法 ----

    /// 保存为 YAML（与 ROS2 标定工具兼容）
    bool save(const std::string& path) const;

    /// 从 YAML 加载
    static std::optional<CalibrationParams> load(const std::string& path);

    /// 验证标定有效性（基线距离、畸变范围、重投影误差）
    ValidationResult validate() const;

    /// 获取 Q 矩阵（深度到视差映射）
    cv::Matx44f getQMatrix() const;
};

}  // namespace stereo_vision_driver
```

---

## 3. 错误处理规范

### 3.1 错误码层次结构

```cpp
namespace stereo_vision_driver {

/**
 * @brief 错误码（层次化，位掩码）
 *
 * 设计原则：
 * - 错误码唯一标识错误位置和类型
 * - 错误码可组合（用 | 运算符）
 * - 有 toString() 用于日志
 */
enum class ErrorCode : uint32_t {
    None            = 0,

    // 严重程度掩码（高16位）
    Severity_MASK   = 0xFFU << 24,
    INFO            = 0x01U << 24,   // 只是信息，不是错误
    WARNING         = 0x02U << 24,   // 警告（可能影响性能）
    ERROR           = 0x04U << 24,   // 操作失败（需要干预）
    FATAL           = 0x08U << 24,   // 致命（设备不可用）

    // 模块掩码（中8位）
    Module_MASK     = 0xFFU << 16,

    // 模块编号
    MODULE_CORE     = 0x01U << 16,
    MODULE_HARDWARE = 0x02U << 16,
    MODULE_SGBM     = 0x03U << 16,
    MODULE_CALIB    = 0x04U << 16,
    MODULE_IMU      = 0x05U << 16,
    MODULE_ROS2     = 0x06U << 16,

    // 具体错误码（低16位）
    // 格式：SEVERITY | MODULE | CODE

    // CORE
    NotInitialized  = ERROR | MODULE_CORE | 0x01,
    InvalidArgument = ERROR | MODULE_CORE | 0x02,
    Timeout         = ERROR | MODULE_CORE | 0x03,
    OutOfMemory     = ERROR | MODULE_CORE | 0x04,

    // HARDWARE
    DeviceNotFound  = ERROR | MODULE_HARDWARE | 0x01,
    DeviceBusy      = ERROR | MODULE_HARDWARE | 0x02,
    I2cFailed       = ERROR | MODULE_HARDWARE | 0x03,
    StreamStartFailed = ERROR | MODULE_HARDWARE | 0x04,
    FrameDropped     = WARNING | MODULE_HARDWARE | 0x10,
    BufferOverflow   = ERROR | MODULE_HARDWARE | 0x05,

    // SGBM
    SGBMInitFailed  = ERROR | MODULE_SGBM | 0x01,
    DisparityFailed = ERROR | MODULE_SGBM | 0x02,

    // CALIBRATION
    CalibInvalid    = ERROR | MODULE_CALIB | 0x01,
    CalibUnstable  = WARNING | MODULE_CALIB | 0x02,

    // IMU
    IMUNotResponding = ERROR | MODULE_IMU | 0x01,
    IMUDataInvalid  = WARNING | MODULE_IMU | 0x10,
};

constexpr ErrorCode operator|(ErrorCode a, ErrorCode b) {
    return static_cast<ErrorCode>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr bool hasError(ErrorCode code, ErrorCode test) {
    return (static_cast<uint32_t>(code) & static_cast<uint32_t>(test)) != 0;
}

/**
 * @brief 错误信息
 */
struct ErrorInfo {
    ErrorCode code = ErrorCode::None;
    std::string message;          // 人类可读信息
    std::string context;          // 额外上下文（函数名/文件名）
    uint64_t timestamp_ns = 0;   // 发生时刻

    std::string toString() const {
        if (code == ErrorCode::None) return "OK";
        char buf[512];
        snprintf(buf, sizeof(buf), "[%s] %s (context=%s, time=%lu)",
                 toString(code), message.c_str(), context.c_str(), timestamp_ns);
        return buf;
    }

    static const char* toString(ErrorCode code);
};

}  // namespace stereo_vision_driver
```

### 3.2 错误处理策略

```
抛出 vs 返回错误码：
  C 接口 / 回调 → 返回 ErrorCode
  C++ API → 抛异常（std::runtime_error 子类）
  ROS2 参数校验 → 断言 + rclcpp 错误日志

异常层次：
  exception
  └── stereo_vision::StereoVisionException (base)
      ├── DeviceException         (硬件相关)
      ├── CalibrationException   (标定相关)
      ├── AlgorithmException      (算法执行失败)
      └── TimeoutException        (操作超时)
```

---

## 4. ROS2 接口规范

### 4.1 话题

| 话题 | 类型 | 频率 | 说明 |
|------|------|------|------|
| `/stereo_camera/left/image_rect` | `sensor_msgs/Image` | @ publish_hz | 左目矫正图像 |
| `/stereo_camera/right/image_rect` | `sensor_msgs/Image` | @ publish_hz | 右目矫正图像 |
| `/stereo_camera/depth` | `sensor_msgs/Image` | @ publish_hz | 深度图（米，32FC1）|
| `/stereo_camera/confidence` | `sensor_msgs/Image` | @ publish_hz | 置信度图（32FC1，0~1）|
| `/stereo_camera/imu` | `sensor_msgs/Imu` | 200Hz | 同步 IMU 数据 |
| `/stereo_camera/status` | `stereo_vision_driver/StereoDeviceStatus` | 1Hz | 设备状态 |
| `/stereo_camera/calibration` | `stereo_vision_driver/StereoCalibrationInfo` | on-change | 标定参数 |

### 4.2 服务

| 服务 | 类型 | 说明 |
|------|------|------|
| `/stereo_camera/calibrate` | `std_srvs/Trigger` | 触发在线标定 |
| `/stereo_camera/load_calibration` | `std_srvs/SetBool` | 加载标定文件 |
| `/stereo_camera/set_exposure` | `std_srvs/SetInt32` | 设置曝光 |
| `/stereo_camera/set_gain` | `std_srvs/SetFloat32` | 设置增益 |
| `/stereo_camera/reset` | `std_srvs/Trigger` | 重置设备 |

### 4.3 参数（ROS2 参数服务器）

```yaml
stereo_camera_node:
  ros__parameters:
    # 发布控制
    publish_hz: 10.0
    publish_depth: true
    publish_confidence: true
    publish_raw_images: false

    # SGBM 参数
    sgbm:
      num_disparities: 128
      block_size: 7
      uniqueness_ratio: 15
      speckle_window_size: 100
      speckle_range: 32
      min_confidence: 0.65

    # 传感器控制
    sensor:
      exposure_us: 5000
      analog_gain_db: 0.0
      hdr_mode: 0
      i2c_bus: 1

    # 硬件
    hardware:
      left_device: "/dev/video0"
      right_device: "/dev/video1"
      width: 3840
      height: 2160

    # IMU
    imu:
      enabled: true
      accel_range: 16
      gyro_range: 2000
```

---

## 5. C API（嵌入式/跨语言）

```c
#ifdef __cplusplus
extern "C" {
#endif

// 不透明句柄
typedef struct SvCameraImpl* SvCameraHandle;

// ---- 生命周期 ----

/**
 * @brief 创建相机实例
 * @param config_json JSON 格式配置
 * @param out_handle 输出句柄
 * @return 0=成功，负值=错误码
 */
int sv_camera_create(const char* config_json, SvCameraHandle* out_handle);

/**
 * @brief 销毁相机实例
 */
int sv_camera_destroy(SvCameraHandle handle);

/**
 * @brief 启动视频流
 */
int sv_camera_start(SvCameraHandle handle);

/**
 * @brief 停止视频流
 */
int sv_camera_stop(SvCameraHandle handle);

// ---- 采集 ----

/**
 * @brief 采集一帧
 * @param handle 句柄
 * @param timeout_ms 超时毫秒
 * @param out_frame 输出帧（调用者分配，传入指针）
 * @return 0=成功，负值=错误码，EAGAIN=超时
 */
int sv_camera_grab(SvCameraHandle handle,
                   uint32_t timeout_ms,
                   SvFrame* out_frame);

/**
 * @brief 释放帧缓冲（必须调用）
 */
int sv_camera_release_frame(SvCameraHandle handle, SvFrame* frame);

// ---- 配置 ----

int sv_camera_set_exposure(SvCameraHandle handle, uint32_t us);
int sv_camera_set_gain(SvCameraHandle handle, float db);
int sv_camera_get_calibration(SvCameraHandle handle, SvCalibration* out_calib);

// ---- 错误 ----
const char* sv_error_string(int error_code);

#ifdef __cplusplus
}
#endif
```

---

## 6. 参数命名逻辑总结

```
命名逻辑：

1. 传感器参数：{property} = {unit}
   exposure_us     (微秒)
   analog_gain_db  (分贝)
   timestamp_ns     (纳秒)

2. 算法参数：{algo}_{property}
   sgbm_num_disparities
   sgbm_block_size
   conf_threshold

3. 深度单位：_m / _mm / _um
   depth_m         (米)
   baseline_mm     (毫米)

4. 布尔参数：use_{verb} / enable_{noun} / disable_{noun}
   use_gpu
   enable_auto_calib

5. 枚举参数：{noun}_mode / {noun}_type
   hdr_mode
   calib_mode
```
