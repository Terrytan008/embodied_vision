// Embodied Vision — ONNXRuntime 置信度推理
// confidence_onnx.cpp

#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <vector>
#include <string>
#include <chrono>
#include <memory>

namespace stereo_vision {

/**
 * @brief ONNXRuntime 置信度推理引擎
 *
 * 加载训练好的 ONNX 模型，执行推理
 * 支持 GPU (CUDA/Metal) / CPU
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

    ConfidenceInference() = default;
    ~ConfidenceInference() = default;

    bool initialize(const Config& config) {
        config_ = config;

        // ---- 创建 ONNX Runtime 环境 ----
        if (config_.use_gpu) {
            // 优先尝试 CUDA
            try {
                OrtCUDAProviderOptions cuda_opts;
                cuda_opts.device_id = config_.gpu_id;
                cuda_opts.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchExhaustive;
                cuda_opts.gpu_mem_limit = 1024ULL * 1024 * 1024;  // 1GB

                session_options_.AppendExecutionProvider_CUDA(cuda_opts);
                provider_ = "CUDA";
            } catch (const std::exception& e) {
                // 回退到 CPU
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

        // 输入
        auto input_name_ptr = session_->GetInputNameAllocated(0, allocator_);
        input_name_ = input_name_ptr.get();
        auto input_shape = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        input_size_ = 1;
        for (auto dim : input_shape) {
            if (dim > 0) input_size_ *= static_cast<size_t>(dim);
        }

        // 输出
        auto output_name_ptr = session_->GetOutputNameAllocated(0, allocator_);
        output_name_ = output_name_ptr.get();

        std::string info = "Confidence ONNX (";
        info += provider_ + ") | ";
        info += "input[" + std::to_string(input_size_) + "] | ";
        info += "threshold=" + std::to_string(config_.confidence_threshold);
        version_ = info;

        return true;
    }

    /**
     * @brief 执行推理
     *
     * @param left  左目灰度图 (CV_8UC1)
     * @param right 右目灰度图 (CV_8UC1)
     * @param disparity 视差图 (CV_32FC1, 像素)
     * @return 置信度图 (CV_32FC1, 0.0~1.0)
     */
    cv::Mat infer(const cv::Mat& left,
                  const cv::Mat& right,
                  const cv::Mat& disparity) {

        // ---- 预处理：resize + normalize + pack to tensor ----
        std::vector<float> input_tensor(input_size_);

        // 归一化到 [0,1]
        cv::Mat left_f, right_f, disp_f;
        left.convertTo(left_f, CV_32FC1, 1.0/255.0);
        right.convertTo(right_f, CV_32FC1, 1.0/255.0);
        disparity.convertTo(disp_f, CV_32FC1, 1.0/255.0);

        // 缩放到网络输入尺寸
        cv::Mat left_r, right_r, disp_r;
        cv::resize(left_f, left_r, cv::Size(config_.img_width, config_.img_height));
        cv::resize(right_f, right_r, cv::Size(config_.img_width, config_.img_height));
        cv::resize(disp_f, disp_r, cv::Size(config_.img_width, config_.img_height));

        // 打包为 NCHW
        // input = [left(1ch), right(1ch), disp(1ch)] → 作为 3 通道
        // ONNX: NCHW = [1, 3, H, W]
        const int H = config_.img_height;
        const int W = config_.img_width;
        const int stride = H * W;

        for (int y = 0; y < H; y++) {
            const float* left_row = left_r.ptr<float>(y);
            const float* right_row = right_r.ptr<float>(y);
            const float* disp_row = disp_r.ptr<float>(y);

            for (int x = 0; x < W; x++) {
                size_t idx = y * W + x;
                input_tensor[idx] = left_row[x];           // Ch 0
                input_tensor[stride + idx] = right_row[x];  // Ch 1
                input_tensor[2*stride + idx] = disp_row[x]; // Ch 2
            }
        }

        // ---- 推理 ----
        std::array<int64_t, 4> input_shape = {1, 3,
            static_cast<int64_t>(H),
            static_cast<int64_t>(W)};

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

        // ---- 后处理：resize 回原尺寸 + 阈值 ----
        cv::Mat confidence_raw(H, W, CV_32FC1, output_data);
        cv::Mat confidence;
        cv::resize(confidence_raw, confidence, left.size());

        // 应用阈值
        // (低于阈值的区域保持原值，用户可自行决定如何处理)
        // confidence.setTo(0, confidence < config_.confidence_threshold);

        return confidence;
    }

    std::string getVersion() const {
        return version_;
    }

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

// 静态初始化
Ort::Env ConfidenceInference::env_{};

}  // namespace stereo_vision
