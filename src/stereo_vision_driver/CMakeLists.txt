cmake_minimum_required(VERSION 3.8)
project(stereo_vision_driver)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -fPIC)
endif()

# ---------- 依赖 ----------
find_package(ament_cmake_auto REQUIRED)
ament_auto_find_build_dependencies()

# OpenCV
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

# ---------- 消息 ----------
rosidl_generate_interfaces(${PROJECT_NAME}
    "msg/StereoCalibrationInfo.msg"
    "msg/StereoDeviceStatus.msg"
)

# ---------- 核心库（独立，无ROS依赖）----------
add_library(stereo_vision_core STATIC
    src/stereo_camera_impl.cpp
    src/stereo_depth.cpp
    src/hardware/nv_csi_capture.cpp
    src/hardware/imu_bmi088.cpp
    src/hardware/hardware_factory.cpp
)
target_include_directories(stereo_vision_core PUBLIC
    include
    include/stereo_vision
    include/stereo_vision/hardware
)
target_link_libraries(stereo_vision_core
    ${OpenCV_LIBS}
    pthread
)

# ---------- ROS2 节点 ----------
rclcpp_components_register_node(stereo_vision_core
    PLUGIN "stereo_vision_driver::StereoCameraNode"
    EXECUTABLE stereo_camera_node
)
ament_auto_target_library(stereo_vision_core)

# ---------- 安装 ----------
ament_auto_package()

# 安装头文件（供外部使用）
install(DIRECTORY include/
    DESTINATION include
)
