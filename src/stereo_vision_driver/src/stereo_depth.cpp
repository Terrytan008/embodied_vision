// Embodied Vision — SGBM 深度计算引擎
// stereo_depth.cpp

#include "stereo_vision/stereo_camera.hpp"
#include "stereo_vision/hardware/camera_types.hpp"

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
#include <vector>
#include <cmath>

namespace stereo_vision {

// ============================================================================
// SGBM 深度计算引擎
// ============================================================================
class SGBMDepthEngine {
public:
    // 置信度估计模式（前置声明）
    enum class ConfidenceMode {
        LeftRightCheck,   // 左右一致性检查
        MatchCost,         // 匹配代价分布
        SobelTexture,      // Sobel梯度纹理
        Combined           // 综合（推荐）
    };

    struct Params {
        // 匹配参数
        int min_disparity = 0;
        int num_disparities = 128;
        int block_size = 7;
        int P1 = 8 * 7 * 7;
        int P2 = 32 * 7 * 7;
        int disp_12_max_diff = -1;
        int uniqueness_ratio = 15;
        int speckle_window_size = 100;
        int speckle_range = 32;
        int pre_filter_cap = 63;
        int pre_filter_size = 5;

        // 置信度参数
        float min_confidence = 0.4f;
        int texture_threshold = 10;

        // 置信度估计模式
        ConfidenceMode confidence_mode = ConfidenceMode::Combined;
    };

    SGBMDepthEngine(const Params& params) : params_(params) {
        initSobelKernel();
    }

    // -------------------------------------------------------------------------
    // 主计算接口
    // -------------------------------------------------------------------------

    /**
     * @brief 计算深度图
     * @param left_raw  左目原始图像（可用于彩色校正）
     * @param right_raw 右目原始图像
     * @param calib     标定参数（用于校正）
     * @param[out] depth_m 输出深度图（米）
     * @param[out] confidence 输出置信度图（0.0~1.0）
     * @param[out] disparity 输出视差图（像素）
     */
    void compute(const cv::Mat& left_raw,
                 const cv::Mat& right_raw,
                 const CalibrationData& calib,
                 cv::Mat& depth_m,
                 cv::Mat& confidence,
                 cv::Mat& disparity) {

        CV_Assert(left_raw.size() == right_raw.size());
        CV_Assert(!calib.left_k.empty() && !calib.t_lr.empty());

        // ---- 预处理：校正 + 灰度化 ----
        cv::Mat left_gray, right_gray;
        preprocess(left_raw, right_raw, calib, left_gray, right_gray);

        // ---- Step 1: 立体校正 ----
        cv::Mat left_rect, right_rect;
        stereoRectify(left_gray, right_gray, calib, left_rect, right_rect);

        // ---- Step 2: SGBM 视差计算 ----
        cv::Mat disp_raw;
        computeDisparity(left_rect, right_rect, disp_raw);

        // ---- Step 3: 视差后处理 ----
        cv::Mat disp_filtered;
        postProcess(disp_raw, disp_filtered, left_rect);

        // ---- Step 4: 视差转深度 ----
        disparityToDepth(disp_filtered, calib, depth_m);

        // ---- Step 5: 置信度估计 ----
        computeConfidence(left_rect, right_rect, disp_filtered, disparity, confidence);

        // ---- Step 6: 失效区域屏蔽 ----
        applySafetyMask(depth_m, confidence, disparity);
    }

private:
    Params params_;

    // ---- 预处理 ----
    void preprocess(const cv::Mat& left_raw,
                    const cv::Mat& right_raw,
                    const CalibrationData& calib,
                    cv::Mat& left_out,
                    cv::Mat& right_out) {
        // 转换为灰度（如果输入是彩色）
        if (left_raw.channels() == 3) {
            cv::cvtColor(left_raw, left_out, cv::COLOR_BGR2GRAY);
            cv::cvtColor(right_raw, right_out, cv::COLOR_BGR2GRAY);
        } else {
            left_out = left_raw.clone();
            right_out = right_raw.clone();
        }

        // 伽马校正（增强暗区纹理）
        // 对于IMX678的HDR场景有帮助
        cv::Mat lookUpTable(1, 256, CV_8UC1);
        uchar* p = lookUpTable.ptr();
        for (int i = 0; i < 256; i++) {
            p[i] = cv::saturate_cast<uchar>(
                255.0 * std::pow(i / 255.0, 0.9)
            );
        }
        cv::LUT(left_out, lookUpTable, left_out);
        cv::LUT(right_out, lookUpTable, right_out);
    }

    // ---- 立体校正 ----
    void stereoRectify(const cv::Mat& left_gray,
                       const cv::Mat& right_gray,
                       const CalibrationData& calib,
                       cv::Mat& left_rect,
                       cv::Mat& right_rect) {
        // 内参矩阵
        cv::Mat K1 = cv::Mat(3, 3, CV_32F, const_cast<float*>(calib.left_k.data()));
        cv::Mat K2 = cv::Mat(3, 3, CV_32F, const_cast<float*>(calib.right_k.data()));

        // 畸变系数
        cv::Mat D1 = cv::Mat(1, 5, CV_32F, const_cast<float*>(calib.left_d.data()));
        cv::Mat D2 = cv::Mat(1, 5, CV_32F, const_cast<float*>(calib.right_d.data()));

        // 外参：从 calib.t_lr (3x4 RT矩阵) 提取 R 和 T
        cv::Mat R(3, 3, CV_32F);
        cv::Mat T(3, 1, CV_32F);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                R.at<float>(i, j) = calib.t_lr[i * 4 + j];
            }
        }
        T.at<float>(0) = calib.t_lr[3];
        T.at<float>(1) = calib.t_lr[7];
        T.at<float>(2) = calib.t_lr[11];

        // 校正矩阵
        cv::Mat R1, R2, P1, P2, Q;
        cv::Rect validRoi;
        cv::stereoRectify(K1, D1, K2, D2, left_gray.size(),
                         R, T, R1, R2, P1, P2, Q,
                         cv::CALIB_ZERO_DISPARITY, -1, left_gray.size(), &validRoi);

        // 校正映射
        cv::Mat map1x, map1y, map2x, map2y;
        cv::initUndistortRectifyMap(K1, D1, R1, P1, left_gray.size(), CV_32FC1,
                                   map1x, map1y);
        cv::initUndistortRectifyMap(K2, D2, R2, P2, right_gray.size(), CV_32FC1,
                                   map2x, map2y);

        // 执行校正
        cv::remap(left_gray, left_rect, map1x, map1y, cv::INTER_LINEAR);
        cv::remap(right_gray, right_rect, map2x, map2y, cv::INTER_LINEAR);

        Q_ = Q.clone();  // 保存Q矩阵用于视差→深度
    }

    // ---- SGBM 视差计算 ----
    void computeDisparity(const cv::Mat& left, const cv::Mat& right, cv::Mat& disparity) {
        // OpenCV的StereoSGBM实现
        cv::Ptr<cv::StereoSGBM> sgbm = cv::StereoSGBM::create(
            params_.min_disparity,
            params_.num_disparities,
            params_.block_size,
            params_.P1,
            params_.P2,
            params_.disp_12_max_diff,
            params_.pre_filter_cap,
            params_.uniqueness_ratio,
            params_.speckle_window_size,
            params_.speckle_range,
            cv::StereoSGBM::MODE_SGBM  // 动态规划模式
        );

        // 左右一致性检查（成本更高但更准确）
        cv::Mat disp_left, disp_right;
        sgbm->compute(left, right, disp_left);

        // 右视图视差
        cv::Ptr<cv::StereoSGBM> sgbm_right = cv::StereoSGBM::create(
            params_.min_disparity,
            params_.num_disparities,
            params_.block_size,
            params_.P1,
            params_.P2,
            params_.disp_12_max_diff,
            params_.pre_filter_cap,
            params_.uniqueness_ratio,
            params_.speckle_window_size,
            params_.speckle_range,
            cv::StereoSGBM::MODE_SGBM
        );
        sgbm_right->compute(right, left, disp_right);

        // 左右一致性检查：|disp_left - disp_right| 应小于阈值
        cv::Mat diff;
        cv::absdiff(disp_left, disp_right, diff);
        diff.convertTo(diff, CV_32F);
        diff = diff / 16.0f;  // OpenCV视差是16.固定点

        // 遮挡区域检测（左目能看到但右目看不到）
        // 右视图的遮挡通常发生在视差较小的区域
        cv::Mat occlusion_mask = (diff > params_.disp_12_max_diff) & (disp_left < disp_right);

        // 将遮挡区域视差设为无效（-1）
        cv::Mat disp_valid = disp_left.clone();
        disp_valid.setTo(-1, occlusion_mask);

        disparity = disp_valid;
    }

    // ---- 后处理 ----
    void postProcess(const cv::Mat& disparity_in, cv::Mat& disparity_out, const cv::Mat& left) {
        disparity_out = disparity_in.clone();

        // 散斑滤波（去除小的孤立区域）
        if (params_.speckle_window_size > 0) {
            cv::Mat speckle_mask;
            cv::filterSpeckles(disparity_out, 0,
                              params_.speckle_window_size,
                              params_.speckle_range,
                              speckle_mask);
        }

        // 中值滤波（去除脉冲噪声）
        cv::medianBlur(disparity_out, disparity_out, 5);

        // 亚像素细化（抛物线插值）
        subpixelRefine(disparity_out, left);
    }

    // ---- 亚像素细化 ----
    void subpixelRefine(cv::Mat& disparity, const cv::Mat& left) {
        // 仅对视差大于0的区域进行亚像素细化
        cv::Mat disp_f = disparity.clone();
        disparity.create(left.size(), CV_32F);
        disparity.setTo(-1.0f);

        const int w = left.cols;
        const int h = left.rows;
        const int d_max = params_.num_disparities;

        for (int y = 1; y < h - 1; y++) {
            for (int x = d_max + 1; x < w - 1; x++) {
                float d = disp_f.at<int16_t>(y, x) / 16.0f;
                if (d <= 0) continue;

                int di = cvRound(d);
                if (di < 1 || di >= d_max - 1) continue;

                // 获取三个像素的匹配代价
                float c0 = getMatchCost(left, x, y, di - 1);
                float c1 = getMatchCost(left, x, y, di);
                float c2 = getMatchCost(left, x, y, di + 1);

                // 抛物线插值
                float denom = c0 + c2 - 2 * c1;
                if (std::abs(denom) > 1e-6) {
                    float delta = (c0 - c2) / (2 * denom);
                    disparity.at<float>(y, x) = d + delta;
                } else {
                    disparity.at<float>(y, x) = d;
                }
            }
        }
    }

    // ---- 匹配代价（绝对差）----
    float getMatchCost(const cv::Mat& left, int x, int y, int d) {
        // 简化的SAD代价
        int sum = 0;
        int count = 0;
        int w = params_.block_size / 2;
        for (int dy = -w; dy <= w; dy++) {
            for (int dx = -w; dx <= w; dx++) {
                int xl = x + dx;
                int xr = x - d + dx;  // 右目对应位置
                if (xr < 0) continue;
                int il = left.at<uint8_t>(y + dy, xl);
                int ir = left.at<uint8_t>(y + dy, xr);
                sum += std::abs(il - ir);
                count++;
            }
        }
        return count > 0 ? static_cast<float>(sum) / count : 255.0f;
    }

    // ---- 视差→深度 ----
    void disparityToDepth(const cv::Mat& disparity, const CalibrationData& calib, cv::Mat& depth) {
        depth.create(disparity.size(), CV_32F);
        depth.setTo(0.0f);

        // Q矩阵：[fx,  0, cx, 0]
        //        [ 0, fy, cy, 0]
        //        [ 0,  0,  0, -B]
        //        [ 0,  0,  1,  0]   (B=基线)
        float fx = calib.left_k[0];
        float cx = calib.left_k[2];
        float B = calib.baseline_mm / 1000.0f;  // mm → m

        for (int y = 0; y < disparity.rows; y++) {
            for (int x = 0; x < disparity.cols; x++) {
                float d = disparity.at<float>(y, x);
                if (d <= 0.0f) continue;

                // Z = fx * B / d
                float Z = fx * B / d;
                if (Z < 0.1f || Z > 10.0f) continue;  // 超出范围

                depth.at<float>(y, x) = Z;
            }
        }
    }

    // ---- 置信度估计 ----
    void computeConfidence(const cv::Mat& left,
                          const cv::Mat& right,
                          const cv::Mat& disp_filtered,
                          const cv::Mat& disparity,
                          cv::Mat& confidence) {
        confidence.create(left.size(), CV_32F);
        confidence.setTo(0.0f);

        switch (params_.confidence_mode) {
            case Params::ConfidenceMode::LeftRightCheck:
                confidenceLRCheck(left, right, disp_filtered, confidence);
                break;
            case Params::ConfidenceMode::MatchCost:
                confidenceMatchCost(left, disp_filtered, confidence);
                break;
            case Params::ConfidenceMode::SobelTexture:
                confidenceSobel(left, disp_filtered, confidence);
                break;
            case Params::ConfidenceMode::Combined:
            default:
                confidenceCombined(left, right, disp_filtered, disparity, confidence);
                break;
        }
    }

    // 左右一致性检查置信度
    void confidenceLRCheck(const cv::Mat&, const cv::Mat&,
                          const cv::Mat& disp_filtered, cv::Mat& conf) {
        // 简化：直接用视差值作为置信度代理
        // 真实实现需要计算左右一致性比率
        disp_filtered.convertTo(conf, CV_32F);
        conf = conf / static_cast<float>(params_.num_disparities);
        cv::threshold(conf, conf, params_.min_confidence, 1.0f, cv::THRESH_BINARY);
    }

    // 匹配代价分布置信度
    void confidenceMatchCost(const cv::Mat& left,
                            const cv::Mat& disp_filtered, cv::Mat& conf) {
        conf.create(left.size(), CV_32F);
        const int w = left.cols;
        const int h = left.rows;

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (disp_filtered.at<float>(y, x) <= 0) {
                    conf.at<float>(y, x) = 0.0f;
                    continue;
                }

                int d = cvRound(disp_filtered.at<float>(y, x));
                if (d < 1 || d >= params_.num_disparities - 1) {
                    conf.at<float>(y, x) = 0.0f;
                    continue;
                }

                // 计算代价分布的方差（平坦区域方差小=低置信）
                float c0 = getMatchCost(left, x, y, d - 1);
                float c1 = getMatchCost(left, x, y, d);
                float c2 = getMatchCost(left, x, y, d + 1);

                float variance = (c0 + c2 - 2*c1);
                float conf_val = std::min(1.0f, std::max(0.0f, variance / 50.0f));
                conf.at<float>(y, x) = conf_val;
            }
        }
    }

    // Sobel纹理置信度
    void confidenceSobel(const cv::Mat& left,
                        const cv::Mat& disp_filtered, cv::Mat& conf) {
        // Sobel梯度模
        cv::Mat grad_x, grad_y;
        cv::Sobel(left, grad_x, CV_32F, 1, 0, 3);
        cv::Sobel(left, grad_y, CV_32F, 0, 1, 3);
        cv::magnitude(grad_x, grad_y, conf);

        // 低纹理区域降置信
        cv::threshold(conf, conf, params_.texture_threshold, 1.0f, cv::THRESH_TRUNC);
        conf /= static_cast<float>(params_.texture_threshold * 2);

        // 无视差区域置零
        conf.setTo(0.0f, disp_filtered <= 0);
    }

    // 综合置信度（推荐）
    void confidenceCombined(const cv::Mat& left,
                           const cv::Mat& right,
                           const cv::Mat& disp_filtered,
                           const cv::Mat& disparity,
                           cv::Mat& conf) {
        // Step 1: Sobel纹理
        cv::Mat conf_sobel;
        confidenceSobel(left, disp_filtered, conf_sobel);

        // Step 2: 匹配代价方差
        cv::Mat conf_cost;
        confidenceMatchCost(left, disp_filtered, conf_cost);

        // Step 3: 唯一性（次优匹配与最优的比值）
        cv::Mat conf_unique;
        computeUniqueness(left, disp_filtered, conf_unique);

        // 综合：几何平均
        cv::Mat conf_combined;
        cv::pow(conf_sobel * conf_cost * conf_unique, 1.0f/3.0f, conf_combined);

        // Step 4: 遮挡区域降权
        // 检测视差跳变（可能是遮挡边界）
        cv::Mat disp_grad_x, disp_grad_y;
        cv::Sobel(disp_filtered, disp_grad_x, CV_32F, 1, 0, 3);
        cv::Sobel(disp_filtered, disp_grad_y, CV_32F, 0, 1, 3);
        cv::Mat disp_grad_mag;
        cv::magnitude(disp_grad_x, disp_grad_y, disp_grad_mag);

        // 视差梯度大的地方降置信（通常是遮挡边界）
        cv::Mat conf_occlusion = 1.0f - cv::min(disp_grad_mag / 5.0f, 1.0f);

        conf = conf_combined.mul(conf_occlusion);

        // 应用阈值
        cv::threshold(conf, conf, params_.min_confidence, 1.0f, cv::THRESH_BINARY);
    }

    // 唯一性置信度
    void computeUniqueness(const cv::Mat& left,
                           const cv::Mat& disp_filtered, cv::Mat& conf) {
        conf.create(left.size(), CV_32F);
        const int w = left.cols;
        const int h = left.rows;

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int d = cvRound(disp_filtered.at<float>(y, x));
                if (d <= 0) {
                    conf.at<float>(y, x) = 0.0f;
                    continue;
                }

                // 简单的次优比
                float best_cost = getMatchCost(left, x, y, d);
                float second_best = 1e9f;

                for (int dd = params_.min_disparity; dd < params_.num_disparities; dd++) {
                    if (dd == d) continue;
                    float c = getMatchCost(left, x, y, dd);
                    if (c < second_best) second_best = c;
                }

                float ratio = second_best / (best_cost + 1e-6f);
                conf.at<float>(y, x) = std::min(1.0f, (ratio - 1.0f) / 10.0f);
            }
        }
    }

    // ---- 安全掩码 ----
    void applySafetyMask(cv::Mat& depth, cv::Mat& conf, cv::Mat& disparity) {
        // 零深度 → 零置信
        for (int y = 0; y < depth.rows; y++) {
            for (int x = 0; x < depth.cols; x++) {
                if (depth.at<float>(y, x) <= 0.0f) {
                    conf.at<float>(y, x) = 0.0f;
                    disparity.at<float>(y, x) = 0.0f;
                }
            }
        }
    }

    // ---- Sobel核预计算 ----
    void initSobelKernel() {
        // 预计算的3x3 Sobel核（可选）
    }

    // ---- 私有成员 ----
    cv::Rect validRoi_;    // 校正后的有效区域
    cv::Mat Q_;            // 视差→深度 Q 矩阵
};

// ============================================================================
// 便捷接口（集成到 StereoCamera）
// ============================================================================

/**
 * @brief SGBM深度计算辅助类
 */
class SGBMDepthHelper {
public:
    SGBMDepthHelper(const SGBMDepthEngine::Params& params = {}) {
        engine_ = std::make_unique<SGBMDepthEngine>(params);
    }

    void compute(const cv::Mat& left_raw,
                 const cv::Mat& right_raw,
                 const CalibrationData& calib,
                 cv::Mat& depth_m,
                 cv::Mat& confidence,
                 cv::Mat& disparity) {
        engine_->compute(left_raw, right_raw, calib, depth_m, confidence, disparity);
    }

    // 带IMU运动畸变校正的版本
    void computeWithIMU(const cv::Mat& left_raw,
                        const cv::Mat& right_raw,
                        const CalibrationData& calib,
                        const IMUData& imu,
                        cv::Mat& depth_m,
                        cv::Mat& confidence,
                        cv::Mat& disparity) {
        // Step 1: IMU运动补偿（校正畸变）
        cv::Mat left_comp, right_comp;
        applyMotionCorrection(left_raw, right_raw, imu, left_comp, right_comp);

        // Step 2: 标准SGBM
        engine_->compute(left_comp, right_comp, calib, depth_m, confidence, disparity);

        // Step 3: 运动状态下降低置信
        if (imu.timestamp_ns > 0) {
            float motion_boost = imu.gyro[0] * imu.gyro[0] +
                                 imu.gyro[1] * imu.gyro[1] +
                                 imu.gyro[2] * imu.gyro[2];
            motion_boost = std::min(1.0f, motion_boost / 0.1f);  // 0.1 rad/s 归一化

            // 运动越快，置信度折扣越大
            conf_scale_.create(confidence.size(), CV_32F);
            conf_scale_.setTo(1.0f - 0.3f * motion_boost);
            confidence = confidence.mul(conf_scale_);
        }
    }

private:
    std::unique_ptr<SGBMDepthEngine> engine_;
    cv::Mat conf_scale_;  // 运动折扣矩阵

    void applyMotionCorrection(const cv::Mat& left_raw,
                               const cv::Mat& right_raw,
                               const IMUData& imu,
                               cv::Mat& left_out,
                               cv::Mat& right_out) {
        // IMU运动补偿（滚动快门畸变校正）
        //
        // 实现思路（简化版，假设IMU坐标系与Camera近似对齐，无重投影误差）：
        // 1. 从 gyro[0..2] (rad/s) 计算曝光时间内的角度增量
        // 2. 用该旋转对图像做逆变换以抵消运动模糊
        //
        // 完整实现需要：IMU→Camera外参矩阵 R_ic, t_ic
        // 用 R_ic 将 gyro 世界坐标系转速映射到Camera坐标系，
        // 再投影到图像平面做逆扭曲。
        //
        // 当前简化：绕Z轴（偏航）的旋转对深度匹配影响最大，
        // 用图像水平剪切来近似补偿。

        (void)right_raw;  // 预留：双目同步畸变校正

        if (imu.timestamp_ns == 0) {
            left_out = left_raw.clone();
            right_out = left_raw.clone();
            return;
        }

        // 假设帧曝光时间 ~33ms（30fps），读取 gyro 角速度
        constexpr float kExposureSec = 0.033f;
        float dyaw   = imu.gyro[2] * kExposureSec;  // Z轴（偏航）
        float dpitch = imu.gyro[1] * kExposureSec;  // Y轴（俯仰）

        // 阈值：微小旋转忽略不计
        const float kThreshold = 0.002f;  // ~0.1度
        if (std::abs(dyaw) < kThreshold && std::abs(dpitch) < kThreshold) {
            left_out = left_raw.clone();
            right_out = left_raw.clone();
            return;
        }

        // ---- 绕Z轴旋转的逆变换（近似）----
        // 纯旋转时，图像上一点 (x,y) 的位移 ≈ focal_length * angle
        // 简化为水平剪切：Δx ≈ dyaw * fy * (y - cy) / fy = dyaw * (y - cy)
        // 更精确做法是对每个像素做旋转矩阵后向映射，这里用 remap()

        cv::Mat map_x, map_y;
        map_x.create(left_raw.size(), CV_32FC1);
        map_y.create(left_raw.size(), CV_32FC1);

        float cx = left_raw.cols / 2.0f;
        float cy = left_raw.rows / 2.0f;

        for (int y = 0; y < left_raw.rows; ++y) {
            for (int x = 0; x < left_raw.cols; ++x) {
                // 前向旋转：x' = x*cos(θ) - y*sin(θ) ≈ x - dyaw*y
                // 逆变换（从扭曲图像求原位置）：
                // x_src = x_dst - dyaw * y_dst
                float x_rot = x + dyaw * (y - cy);
                float y_rot = y - dpitch * (x - cx);
                map_x.at<float>(y, x) = x_rot;
                map_y.at<float>(y, x) = y_rot;
            }
        }

        cv::remap(left_raw, left_out, map_x, map_y, cv::INTER_LINEAR,
                  cv::BORDER_CONSTANT);
        right_out = left_out;  // 同步左右目（假设相同运动）
    }
};

}  // namespace stereo_vision
