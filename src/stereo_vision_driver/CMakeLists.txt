cmake_minimum_required(VERSION 3.8)
project(stereo_vision_driver)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------- 依赖 ----------
find_package(ament_cmake_auto REQUIRED)
ament_auto_find_build_dependencies()

# ---------- 消息 ----------
rosidl_generate_interfaces(${PROJECT_NAME}
    "msg/StereoCalibrationInfo.msg"
    "msg/StereoDeviceStatus.msg"
)

# ---------- 头文件 ----------
include_directories(
    include
    ${OpenCV_INCLUDE_DIRS}
)

# ---------- 库 ----------
add_library(stereo_vision_lib STATIC
    src/stereo_camera_impl.cpp
)
ament_auto_target_library(stereo_vision_lib)

target_link_libraries(stereo_vision_lib
    ${OpenCV_LIBS}
)

# ---------- 节点 ----------
rclcpp_components_register_node(stereo_vision_lib
    PLUGIN "stereo_vision_driver::StereoCameraNode"
    EXECUTABLE stereo_camera_node
)

# ---------- 测试 ----------
if(BUILD_TESTING)
    find_package(ament_lint_auto REQUIRED)
    ament_lint_auto_find_test_dependencies()
endif()

# ---------- 导出 ----------
ament_auto_package()
