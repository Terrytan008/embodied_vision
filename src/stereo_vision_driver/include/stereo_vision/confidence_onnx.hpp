// Embodied Vision — ONNX 置信度推理引擎
// confidence_onnx.hpp
//
// 完整实现在 confidence_onnx.cpp
// 编译条件：ENABLE_ONNXRUNTIME

#pragma once

#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <memory>
#include <array>
#include <vector>

namespace stereo_vision {

/**
 * @brief ONNX Runtime 置信度推理引擎
 *
 * 加载训练好的轻量 CNN 模型，执行置信度预测 (0.0~1.0)
 * 输入: 左目灰度图 + 右目灰度图 + 视差图 (3通道)
 * 输出: 每像素置信度图
 */
class ConfidenceInference {
public:
    struct Config {
        std::string model_path;       // ONNX 模型路径
        int img_height = 384;
        int img_width = 1280;
        float confidence_threshold = 0.65f;
        bool use_gpu = true;
        int gpu_id = 0;
    };

    ConfidenceInference();
    ~ConfidenceInference();

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

    std::string getVersion() const { return version_; }

private:
    Config config_;
    std::string provider_ = "CPU";
    std::string version_;

    // ONNX Runtime
    static Ort::Env env_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    Ort::AllocatorWithDefaultOptions allocator_;
    std::string input_name_;
    std::string output_name_;
    size_t input_size_ = 0;
};

}  // namespace stereo_vision
