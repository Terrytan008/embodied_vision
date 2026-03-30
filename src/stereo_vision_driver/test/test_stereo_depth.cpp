// Embodied Vision — SGBM 深度引擎单元测试
// test_stereo_depth.cpp

#include <gtest/gtest.h>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include "stereo_vision/stereo_depth.hpp"
#include "stereo_vision/stereo_camera.hpp"

using stereo_vision::SGBMDepthHelper;
using stereo_vision::CalibrationData;
using stereo_vision::SGBMParams;

// ---- 测试辅助 ----
static cv::Mat makeTestImage(int w, int h, int offset) {
    cv::Mat img(h, w, CV_8UC1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            img.at<uchar>(y, x) = static_cast<uchar>(
                128 + 64 * std::sin(2 * CV_PI * x / 80.0 + offset)
                           + 32 * std::cos(2 * CV_PI * y / 60.0));
        }
    }
    return img;
}

static stereo_vision::CalibrationData makeTestCalib() {
    stereo_vision::CalibrationData c;
    c.left_k = {1194.0f, 0.0f, 960.0f,
                 0.0f, 1194.0f, 540.0f,
                 0.0f, 0.0f, 1.0f};
    c.right_k = c.left_k;
    c.baseline_mm = 80.0f;
    c.recalib_confidence = 0.95f;
    c.recalib_timestamp_ns = 0;
    return c;
}

// ---- 测试：深度计算输出尺寸 ----
TEST(SGBMDepth, OutputSizeMatchesInput) {
    SGBMParams params;
    params.num_disparities = 128;
    params.block_size = 7;
    SGBMDepthHelper engine(params);

    int w = 640, h = 480;
    cv::Mat left = makeTestImage(w, h, 0);
    cv::Mat right = makeTestImage(w, h, 10);  // 视差偏移模拟
    cv::Mat depth, conf, disp;
    stereo_vision::CalibrationData calib = makeTestCalib();

    engine.compute(left, right, calib, depth, conf, disp);

    EXPECT_EQ(depth.rows, h);
    EXPECT_EQ(depth.cols, w);
    EXPECT_EQ(conf.rows, h);
    EXPECT_EQ(conf.cols, w);
    EXPECT_EQ(disp.rows, h);
    EXPECT_EQ(disp.cols, w);
    EXPECT_EQ(depth.type(), CV_32FC1);
    EXPECT_EQ(conf.type(), CV_32FC1);
}

// ---- 测试：置信度阈值过滤 ----
TEST(SGBMDepth, ConfidenceThreshold) {
    SGBMParams params;
    params.num_disparities = 64;
    params.block_size = 5;
    params.min_confidence = 0.4f;
    SGBMDepthHelper engine(params);

    cv::Mat left = makeTestImage(320, 240, 0);
    cv::Mat right = makeTestImage(320, 240, 5);
    cv::Mat depth, conf, disp;
    engine.compute(left, right, makeTestCalib(), depth, conf, disp);

    // 置信度应在 [0,1] 范围
    for (int y = 0; y < conf.rows; ++y) {
        for (int x = 0; x < conf.cols; ++x) {
            float c = conf.at<float>(y, x);
            EXPECT_GE(c, 0.0f);
            EXPECT_LE(c, 1.0f);
        }
    }

    // 无效深度对应零置信
    for (int y = 0; y < depth.rows; ++y) {
        for (int x = 0; x < depth.cols; ++x) {
            if (depth.at<float>(y, x) <= 0.0f) {
                EXPECT_EQ(conf.at<float>(y, x), 0.0f);
            }
        }
    }
}

// ---- 测试：深度物理范围 ----
TEST(SGBMDepth, DepthPhysicalRange) {
    SGBMParams params;
    params.num_disparities = 128;
    SGBMDepthHelper engine(params);

    cv::Mat left = makeTestImage(320, 240, 0);
    cv::Mat right = makeTestImage(320, 240, 3);
    cv::Mat depth, conf, disp;
    engine.compute(left, right, makeTestCalib(), depth, conf, disp);

    // 有深度值的像素应在 [0.1m, 10m] 范围内
    for (int y = 0; y < depth.rows; ++y) {
        for (int x = 0; x < depth.cols; ++x) {
            float d = depth.at<float>(y, x);
            if (d > 0.0f) {
                EXPECT_GE(d, 0.1f);
                EXPECT_LE(d, 10.0f);
            }
        }
    }
}

// ---- 测试：IMU 运动补偿不崩溃 ----
TEST(SGBMDepth, MotionCompensationNoCrash) {
    SGBMParams params;
    params.num_disparities = 64;
    SGBMDepthHelper engine(params);

    cv::Mat left = makeTestImage(320, 240, 0);
    cv::Mat right = makeTestImage(320, 240, 5);
    stereo_vision::IMUData imu{};
    imu.gyro[0] = 0.01f; imu.gyro[1] = 0.005f; imu.gyro[2] = 0.02f;
    imu.accel[0] = 0.1f; imu.accel[1] = 0.2f; imu.accel[2] = 9.8f;
    imu.timestamp_ns = 1234567890;

    cv::Mat depth, conf, disp;
    // 不应崩溃
    EXPECT_NO_THROW({
        engine.computeWithIMU(left, right, makeTestCalib(), imu,
                             depth, conf, disp);
    });
}

// ---- 测试：视差→深度 公式 ----
TEST(SGBMDepth, DisparityToDepthFormula) {
    SGBMParams params;
    params.num_disparities = 128;
    SGBMDepthHelper engine(params);

    cv::Mat left = makeTestImage(640, 480, 0);
    cv::Mat right = makeTestImage(640, 480, 8);
    cv::Mat depth, conf, disp;
    engine.compute(left, right, makeTestCalib(), depth, conf, disp);

    // disparity > 0 的像素，depth = fx * B / d
    float fx = 1194.0f;
    float B = 0.080f;  // 80mm → m
    int valid = 0;
    for (int y = 0; y < depth.rows; ++y) {
        for (int x = 0; x < depth.cols; ++x) {
            float d = disp.at<float>(y, x);
            float z = depth.at<float>(y, x);
            if (d > 0.0f && z > 0.0f) {
                float expected_z = fx * B / d;
                EXPECT_NEAR(z, expected_z, 0.5f) << "at (" << x << "," << y << ")";
                valid++;
            }
        }
    }
    EXPECT_GT(valid, 10);  // 至少有一些有效匹配
}

// ---- 测试：置信度辅助函数 ----
TEST(ConfidenceUtil, ConfidenceMask) {
    cv::Mat conf(100, 100, CV_32FC1);
    conf.setTo(0.5f);
    conf(cv::Rect(20, 20, 30, 30)).setTo(0.9f);

    cv::Mat mask_high = stereo_vision::confidenceMask(
        conf, stereo_vision::ConfidenceLevel::HIGH);
    cv::Mat mask_med = stereo_vision::confidenceMask(
        conf, stereo_vision::ConfidenceLevel::MEDIUM);

    // HIGH (≥0.65): 只有 0.9 区域非零
    EXPECT_GT(cv::countNonZero(mask_high(cv::Rect(20,20,30,30))), 0);
    EXPECT_EQ(cv::countNonZero(mask_high(cv::Rect(0,0,20,20))), 0);

    // MEDIUM (≥0.40): 大部分区域非零
    EXPECT_GT(cv::countNonZero(mask_med), 0);
}

TEST(ConfidenceUtil, ClassifyConfidence) {
    EXPECT_EQ(stereo_vision::classifyConfidence(0.90f),
              stereo_vision::ConfidenceLevel::VERY_HIGH);
    EXPECT_EQ(stereo_vision::classifyConfidence(0.70f),
              stereo_vision::ConfidenceLevel::HIGH);
    EXPECT_EQ(stereo_vision::classifyConfidence(0.45f),
              stereo_vision::ConfidenceLevel::MEDIUM);
    EXPECT_EQ(stereo_vision::classifyConfidence(0.10f),
              stereo_vision::ConfidenceLevel::LOW);
    EXPECT_EQ(stereo_vision::classifyConfidence(0.0f),
              stereo_vision::ConfidenceLevel::INVALID);
}

TEST(ConfidenceUtil, ValidateDepthSafety) {
    cv::Mat safe(100, 100, CV_32FC1, cv::Scalar(1.5f));
    EXPECT_TRUE(stereo_vision::validateDepthSafety(safe, 50.0f));

    // 深度跳变超过限制
    cv::Mat unsafe(100, 100, CV_32FC1, cv::Scalar(1.5f));
    unsafe.at<float>(50, 50) = 5.0f;  // 3.5m 跳变在33ms内 → 106m/s > 50
    EXPECT_FALSE(stereo_vision::validateDepthSafety(unsafe, 50.0f));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
