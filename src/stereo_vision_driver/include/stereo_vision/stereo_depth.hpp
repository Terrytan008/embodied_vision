// Embodied Vision — SGBM 深度计算引擎
// stereo_depth.hpp

#pragma once

#include "stereo_camera.hpp"

namespace stereo_vision {

// SGBM 深度引擎参数
struct SGBMParams {
    int min_disparity = 0;
    int num_disparities = 128;   // 16倍数，128足够中等基线80mm
    int block_size = 7;           // 奇数，7x7窗口

    // 平滑惩罚
    int P1 = 8 * 7 * 7;
    int P2 = 32 * 7 * 7;

    int disp_12_max_diff = -1;    // 左右一致性容差
    int uniqueness_ratio = 15;   // 唯一性阈值%
    int speckle_window = 100;     // 散斑滤波窗口
    int speckle_range = 32;      // 散斑范围

    // 置信度
    float min_confidence = 0.65f;
    int texture_threshold = 10;   // Sobel纹理阈值

    // 模式
    enum class ConfidenceMode {
        LeftRightCheck,   // 左右一致性
        MatchCost,       // 匹配代价方差
        SobelTexture,    // Sobel梯度
        Combined         // 综合（推荐）
    };
    ConfidenceMode mode = ConfidenceMode::Combined;
};

/**
 * @brief SGBM 深度计算器
 *
 * 使用 Semi-Global Matching 计算深度图
 *
 * 流程：
 *   原始图像 → 预处理 → 立体校正 → SGBM视差 →
 *   后处理(散斑/亚像素) → 视差→深度 → 置信度估计
 */
class SGBMDepthHelper {
public:
    explicit SGBMDepthHelper(const SGBMParams& params = SGBMParams{});
    ~SGBMDepthHelper() = default;

    /**
     * @brief 计算深度图
     *
     * @param left_raw   左目原始图像（灰度或彩色）
     * @param right_raw  右目原始图像
     * @param calib      标定参数
     * @param[out] depth_m    深度图（米）
     * @param[out] confidence 置信度图（0.0~1.0）
     * @param[out] disparity   视差图（像素）
     */
    void compute(const cv::Mat& left_raw,
                  const cv::Mat& right_raw,
                  const CalibrationData& calib,
                  cv::Mat& depth_m,
                  cv::Mat& confidence,
                  cv::Mat& disparity);

    /**
     * @brief 带IMU运动补偿的深度计算
     *
     * 使用IMU数据校正运动畸变
     */
    void computeWithIMU(const cv::Mat& left_raw,
                         const cv::Mat& right_raw,
                         const CalibrationData& calib,
                         const IMUData& imu,
                         cv::Mat& depth_m,
                         cv::Mat& confidence,
                         cv::Mat& disparity);

private:
    SGBMParams params_;
    struct SGBMDepthEngine;  // 前向声明（实现在 stereo_depth.cpp）
    std::unique_ptr<SGBMDepthEngine> engine_;
};

}  // namespace stereo_vision
