// Embodied Vision — ONNXRuntime 置信度推理
// confidence_onnx.cpp

#include "stereo_vision/confidence_onnx.hpp"

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>
#include <chrono>
#include <memory>

namespace stereo_vision {

// ============================================================================
// 静态成员
// ============================================================================
Ort::Env ConfidenceInference::env_{};

// ============================================================================
// 构造 / 析构
// ============================================================================
ConfidenceInference::ConfidenceInference() = default;
ConfidenceInference::~ConfidenceInference() = default;

// ============================================================================
// 初始化
// ============================================================================
bool ConfidenceInference::initialize(const Config& config) {
    config_ = config;

    // ---- 创建 ONNX Runtime 环境 ----
    if (config_.use_gpu) {
        try {
            OrtCUDAProviderOptions cuda_opts{};
            cuda_opts.device_id = config_.gpu_id;
            cuda_opts.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchExhaustive;
            cuda_opts.gpu_mem_limit = 1024ULL * 1024 * 1024;  // 1GB

            session_options_.AppendExecutionProvider_CUDA(cuda_opts);
            provider_ = "CUDA";
        } catch (const std::exception&) {
            session_options_.SetIntraOpNumThreads(4);
            provider_ = "CPU";
        }
    } else {
        session_options_.SetIntraOpNumThreads(4);
        provider_ = "CPU";
    }

    // ---- 创建 Session ----
    session_ = std::make_unique<Ort::Session>(env_, config_.model_path.c_str(),
                                               session_options_);
    if (!session_) {
        return false;
    }

    // ---- 获取输入/输出信息 ----
    size_t num_input_nodes = session_->GetInputCount();
    size_t num_output_nodes = session_->GetOutputCount();

    if (num_input_nodes < 1 || num_output_nodes < 1) {
        return false;
    }

    auto input_name_ptr = session_->GetInputNameAllocated(0, allocator_);
    input_name_ = input_name_ptr.get();

    auto input_shape = session_->GetInputTypeInfo(0)
                           .GetTensorTypeAndShapeInfo()
                           .GetShape();
    input_size_ = 1;
    for (auto dim : input_shape) {
        if (dim > 0) input_size_ *= static_cast<size_t>(dim);
    }

    auto output_name_ptr = session_->GetOutputNameAllocated(0, allocator_);
    output_name_ = output_name_ptr.get();

    version_ = "Confidence ONNX (" + provider_ + ") | " +
               "input[" + std::to_string(input_size_) + "] | " +
               "threshold=" + std::to_string(config_.confidence_threshold);

    return true;
}

// ============================================================================
// 推理
// ============================================================================
cv::Mat ConfidenceInference::infer(const cv::Mat& left,
                                  const cv::Mat& right,
                                  const cv::Mat& disparity) {
    // ---- 预处理：resize + normalize + pack to tensor ----
    std::vector<float> input_tensor(input_size_);

    cv::Mat left_f, right_f, disp_f;
    left.convertTo(left_f, CV_32FC1, 1.0 / 255.0);
    right.convertTo(right_f, CV_32FC1, 1.0 / 255.0);
    disparity.convertTo(disp_f, CV_32FC1, 1.0 / 255.0);

    cv::Mat left_r, right_r, disp_r;
    cv::resize(left_f, left_r,
               cv::Size(config_.img_width, config_.img_height));
    cv::resize(right_f, right_r,
               cv::Size(config_.img_width, config_.img_height));
    cv::resize(disp_f, disp_r,
               cv::Size(config_.img_width, config_.img_height));

    const int H = config_.img_height;
    const int W = config_.img_width;
    const size_t stride = static_cast<size_t>(H) * W;

    for (int y = 0; y < H; ++y) {
        const float* left_row = left_r.ptr<float>(y);
        const float* right_row = right_r.ptr<float>(y);
        const float* disp_row = disp_r.ptr<float>(y);
        for (int x = 0; x < W; ++x) {
            size_t idx = static_cast<size_t>(y) * W + x;
            input_tensor[idx] = left_row[x];
            input_tensor[stride + idx] = right_row[x];
            input_tensor[2 * stride + idx] = disp_row[x];
        }
    }

    // ---- 推理 ----
    std::array<int64_t, 4> input_shape = {
        1, 3,
        static_cast<int64_t>(H),
        static_cast<int64_t>(W)
    };

    auto memory_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    auto input_tensor_ort = Ort::Value::CreateTensor<float>(
        memory_info,
        input_tensor.data(), input_tensor.size(),
        input_shape.data(), input_shape.size());

    auto output_tensor_ort = session_->Run(
        Ort::RunOptions{nullptr},
        &input_name_, &input_tensor_ort, 1,
        &output_name_, 1);

    float* output_data = output_tensor_ort[0].GetTensorMutableData<float>();

    // ---- 后处理：resize 回原尺寸 ----
    cv::Mat confidence_raw(H, W, CV_32FC1, output_data);
    cv::Mat confidence;
    cv::resize(confidence_raw, confidence, left.size());

    return confidence;
}

}  // namespace stereo_vision
