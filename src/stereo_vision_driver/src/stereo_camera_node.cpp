// Embodied Vision — ROS2 驱动节点
// stereo_camera_node.cpp

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_components/register_node_macro.hpp>

#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <image_transport/image_transport.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "stereo_vision/stereo_camera.hpp"

namespace stereo_vision_driver {

class StereoCameraNode : public rclcpp::Node {
public:
    explicit StereoCameraNode(const rclcpp::NodeOptions& options)
        : rclcpp::Node("stereo_camera_node", options) {

        // ---------- 参数加载 ----------
        this->declare_parameter("publish_hz", 10);
        this->declare_parameter("confidence_threshold", 0.65f);
        this->declare_parameter("depth_min_m", 0.1f);
        this->declare_parameter("depth_max_m", 10.0f);
        this->declare_parameter("exposure_mode", std::string("auto"));
        this->declare_parameter("hdr_mode", std::string("hdr_x2"));

        int publish_hz = this->get_parameter("publish_hz").as_int();
        float confidence_threshold =
            this->get_parameter("confidence_threshold").as_float();

        // ---------- 构建配置 ----------
        stereo_vision::CameraConfig config;
        config.publish_hz = publish_hz;
        config.confidence_threshold = confidence_threshold;
        config.depth_min_m = this->get_parameter("depth_min_m").as_float();
        config.depth_max_m = this->get_parameter("depth_max_m").as_float();
        config.publish_left = true;
        config.publish_right = true;
        config.publish_depth = true;
        config.publish_confidence = true;

        std::string exposure_mode =
            this->get_parameter("exposure_mode").as_string();
        if (exposure_mode == "manual") {
            config.exposure_mode = stereo_vision::CameraConfig::ExposureMode::Manual;
        }

        std::string hdr_mode = this->get_parameter("hdr_mode").as_string();
        if (hdr_mode == "x4") config.hdr_mode = stereo_vision::CameraConfig::HdrMode::X4;
        else if (hdr_mode == "off") config.hdr_mode = stereo_vision::CameraConfig::HdrMode::Off;

        // ---------- 创建设备 ----------
        camera_ = stereo_vision::StereoCamera::create(config, this);
        if (!camera_) {
            RCLCPP_FATAL(this->get_logger(), "无法创建设备实例");
            throw std::runtime_error("StereoCamera::create() failed");
        }

        RCLCPP_INFO(this->get_logger(), "Embodied Vision 驱动启动: %s",
                    camera_->getVersion().c_str());

        // ---------- 发布者 ----------
        img_transport_ = std::make_unique<image_transport::ImageTransport>(
            *this);

        pub_left_ = img_transport_->advertise("left/image_rect", 1);
        pub_right_ = img_transport_->advertise("right/image_rect", 1);
        pub_depth_ = img_transport_->advertise("depth", 1);
        pub_confidence_ = img_transport_->advertise("confidence", 1);

        pub_imu_ = this->create_publisher<sensor_msgs::msg::Imu>("imu", 10);

        // ---------- 定时器 ----------
        int interval_ms = 1000 / std::max(publish_hz, 1);
        timer_ = this->create_wall_timer(
            std::chrono::milliseconds(interval_ms),
            [this]() { this->onTimer(); }
        );

        // ---------- 重标定服务 ----------
        srv_recalib_ = this->create_service<std_srvs::srv::Trigger>(
            "~/recalibrate",
            [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>&,
                   std::shared_ptr<std_srvs::srv::Trigger::Response>& response) {
                RCLCPP_INFO(this->get_logger(), "触发在线标定重收敛...");
                camera_->triggerRecalibration();
                response->success = true;
                response->message = "重新标定已触发";
            }
        );
    }

private:
    void onTimer() {
        auto frame = camera_->grabFrame(100);
        if (!frame) {
            RCLCPP_WARN_THROTTLE(this->get_logger(), *this->get_clock(), 1000,
                                 "grabFrame 超时");
            return;
        }

        auto stamp = this->get_clock()->now();

        // 左目图像
        if (pub_left_.getNumSubscribers() > 0) {
            sensor_msgs::msg::Image msg;
            msg.header.stamp = stamp;
            msg.header.frame_id = "stereo_left";
            msg.height = frame->left_rect.rows;
            msg.width = frame->left_rect.cols;
            msg.encoding = frame->left_rect.channels() == 3 ? "rgb8" : "mono8";
            msg.step = static_cast<uint32_t>(frame->left_rect.step);
            msg.data.assign(frame->left_rect.datastart,
                           frame->left_rect.dataend);
            pub_left_.publish(msg);
        }

        // 右目图像
        if (pub_right_.getNumSubscribers() > 0) {
            sensor_msgs::msg::Image msg;
            msg.header.stamp = stamp;
            msg.header.frame_id = "stereo_right";
            msg.height = frame->right_rect.rows;
            msg.width = frame->right_rect.cols;
            msg.encoding = frame->right_rect.channels() == 3 ? "rgb8" : "mono8";
            msg.step = static_cast<uint32_t>(frame->right_rect.step);
            msg.data.assign(frame->right_rect.datastart,
                           frame->right_rect.dataend);
            pub_right_.publish(msg);
        }

        // 深度图
        if (pub_depth_.getNumSubscribers() > 0) {
            sensor_msgs::msg::Image msg;
            msg.header.stamp = stamp;
            msg.header.frame_id = "stereo_depth";
            msg.height = frame->depth_m.rows;
            msg.width = frame->depth_m.cols;
            msg.encoding = "32FC1";
            msg.step = static_cast<uint32_t>(frame->depth_m.step);
            msg.data.assign(
                reinterpret_cast<const uint8_t*>(frame->depth_m.datastart),
                reinterpret_cast<const uint8_t*>(frame->depth_m.dataend)
            );
            pub_depth_.publish(msg);
        }

        // 置信度图
        if (pub_confidence_.getNumSubscribers() > 0) {
            sensor_msgs::msg::Image msg;
            msg.header.stamp = stamp;
            msg.header.frame_id = "stereo_confidence";
            msg.height = frame->confidence.rows;
            msg.width = frame->confidence.cols;
            msg.encoding = "32FC1";
            msg.step = static_cast<uint32_t>(frame->confidence.step);
            msg.data.assign(
                reinterpret_cast<const uint8_t*>(frame->confidence.datastart),
                reinterpret_cast<const uint8_t*>(frame->confidence.dataend)
            );
            pub_confidence_.publish(msg);
        }

        // IMU
        if (pub_imu_ && pub_imu_->get_subscription_count() > 0 && frame->imu.has_value()) {
            sensor_msgs::msg::Imu imu_msg;
            imu_msg.header.stamp = stamp;
            imu_msg.header.frame_id = "stereo_imu";
            const auto& imu = frame->imu.value();
            imu_msg.angular_velocity.x = imu.gyro[0];
            imu_msg.angular_velocity.y = imu.gyro[1];
            imu_msg.angular_velocity.z = imu.gyro[2];
            imu_msg.linear_acceleration.x = imu.accel[0];
            imu_msg.linear_acceleration.y = imu.accel[1];
            imu_msg.linear_acceleration.z = imu.accel[2];
            pub_imu_->publish(imu_msg);
        }
    }

    std::unique_ptr<stereo_vision::StereoCamera> camera_;
    std::unique_ptr<image_transport::ImageTransport> img_transport_;

    image_transport::Publisher pub_left_;
    image_transport::Publisher pub_right_;
    image_transport::Publisher pub_depth_;
    image_transport::Publisher pub_confidence_;
    rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr pub_imu_;

    rclcpp::TimerBase::SharedPtr timer_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr srv_recalib_;
};

}  // namespace stereo_vision_driver

RCLCPP_COMPONENTS_REGISTER_NODE(stereo_vision_driver::StereoCameraNode)
