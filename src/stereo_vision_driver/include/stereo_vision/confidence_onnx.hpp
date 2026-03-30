// Embodied Vision — ONNX 置信度推理引擎
// confidence_onnx.hpp
//
// 完整实现在 confidence_onnx.cpp
// 编译条件：ENABLE_ONNXRUNTIME

#pragma once

#include <string>
#include <memory>
#include <opencv2/opencv.hpp>

namespace stereo_vision {

/**
 * @brief ONNX Runtime 置信度推理引擎
 *
 * 使用训练好的轻量 CNN 对每像素预测置信度 (0.0~1.0)
 * 输入: 左目灰度图 + 右目灰度图 + 视差图
 * 输出: 每像素置信度图
 */
class ConfidenceInference {
public:
    struct Config {
        std::string model_path;
        int img_height = 384;
        int img_width = 1280;
        float confidence_threshold = 0.65f;
        bool use_gpu = true;
        int gpu_id = 0;
    };

    ConfidenceInference() = default;
    ~ConfidenceInference() = default;

    bool initialize(const Config& config);

    /**
     * @brief 执行推理
     * @param left      左目灰度图 (CV_8UC1)
     * @param right     右目灰度图 (CV_8UC1)
     * @param disparity 视差图 (CV_32FC1, 像素)
     * @return 置信度图 (CV_32FC1, 0.0~1.0)
     */
    cv::Mat infer(const cv::Mat& left,
                  const cv::Mat& right,
                  const cv::Mat& disparity);

    std::string getProvider() const { return provider_; }

private:
    Config config_;
    std::string provider_ = "CPU";
};

}  // namespace stereo_vision
