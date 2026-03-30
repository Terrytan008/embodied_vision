cmake_minimum_required(VERSION 3.8)
project(stereo_vision_driver)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -fPIC)
endif()

# ========== 选项 ==========
option(ENABLE_NVMEDIA "启用 NVIDIA NvMedia CSI-2 捕获 (需要 DriveOS/JetPack SDK)" OFF)
option(ENABLE_ONNXRUNTIME "启用 ONNXRuntime 置信度推理" ON)

# ========== 依赖 ----------
find_package(ament_cmake_auto REQUIRED)
ament_auto_find_build_dependencies()

# 消息包（stereo_vision_msgs 独立包）
find_package(stereo_vision_msgs REQUIRED)

# OpenCV
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

# CUDA (可选)
find_package(CUDA QUIET)
if(CUDA_FOUND)
    message(STATUS "CUDA found: ${CUDA_VERSION}")
    include_directories(${CUDA_INCLUDE_DIRS})
    add_definitions(-DENABLE_CUDA)
endif()

# ========== 核心库（无ROS依赖）----------
set(HW_SOURCES
    src/stereo_camera_impl.cpp
    src/stereo_depth.cpp
    src/hardware/v4l2_capture.cpp          # V4L2 CSI-2 捕获
    src/hardware/imu_bmi088.cpp             # BMI088 IMU
    src/hardware/hardware_selector.cpp      # 平台自动选择
)

# NvMedia (可选，需要 DriveOS SDK)
if(ENABLE_NVMEDIA)
    if(CUDA_FOUND)
        add_definitions(-DENABLE_NVMEDIA)
        list(APPEND HW_SOURCES
            src/hardware/nv_nvmedia_capture.cpp
        )
        message(STATUS "NvMedia CSI-2 捕获: 已启用")
    else()
        message(WARNING "ENABLE_NVMEDIA=ON 但未找到 CUDA，NvMedia 将被禁用")
    endif()
endif()

# ONNXRuntime (可选)
if(ENABLE_ONNXRUNTIME)
    find_package(ONNXRuntime QUIET)
    if(ONNXRuntime_FOUND)
        add_definitions(-DENABLE_ONNXRUNTIME)
        include_directories(${ONNXRUNTIME_INCLUDE_DIR})
        list(APPEND HW_SOURCES
            src/hardware/confidence_onnx.cpp
        )
        message(STATUS "ONNXRuntime 置信度推理: 已启用")
    else()
        message(STATUS "ONNXRuntime 未找到，置信度NN推理将被禁用")
    endif()
endif()

add_library(stereo_vision_core STATIC ${HW_SOURCES})

target_include_directories(stereo_vision_core PUBLIC
    include
    include/stereo_vision
    include/stereo_vision/hardware
)
target_link_libraries(stereo_vision_core
    ${OpenCV_LIBS}
    pthread
    ${CUDA_LIBRARIES}
)

if(ONNXRUNTIME_FOUND)
    target_link_libraries(stereo_vision_core ${ONNXRUNTIME_LIBRARIES})
endif()

# ========== ROS2 节点 ----------
rclcpp_components_register_node(stereo_vision_core
    PLUGIN "stereo_vision_driver::StereoCameraNode"
    EXECUTABLE stereo_camera_node
)
ament_auto_target_library(stereo_vision_core)

# ========== 测试 ----------
if(BUILD_TESTING)
    find_package(ament_lint_auto REQUIRED)
    ament_lint_auto_find_test_dependencies()

    # GTest 单元测试
    find_package(GTest QUIET)
    if(GTest_FOUND)
        enable_testing()
        add_executable(test_stereo_depth
            test/test_stereo_depth.cpp
        )
        target_include_directories(test_stereo_depth PRIVATE
            include
            include/stereo_vision
            ${OpenCV_INCLUDE_DIRS}
        )
        # 链接完整库（包含 SGBM + ONNX 置信度）
        target_link_libraries(test_stereo_depth
            stereo_vision_core
            ${OpenCV_LIBS}
            pthread
            GTest::GTest
            GTest::Main
        )
        ament_add_test(test_stereo_depth
            GENERATE_RESULT_FOR_RETURN_CODE
            COMMAND $<TARGET_FILE:test_stereo_depth>
            ENV ${colcon_test_env}
        )
        message(STATUS "GTest 单元测试: 已启用")
    else()
        message(STATUS "GTest 未找到，跳过单元测试")
    endif()
endif()

# ========== 安装 ----------
ament_auto_package()

install(DIRECTORY include/
    DESTINATION include
)

install(DIRECTORY launch/
    DESTINATION share/${PROJECT_NAME}/launch
)
