cmake_minimum_required(VERSION 3.8)
project(stereo_vision_msgs)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(std_msgs REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/StereoCalibrationInfo.msg"
  "msg/StereoDeviceStatus.msg"
  DEPENDENCIES std_msgs
)

ament_export_dependencies(rosidl_default_generators)

ament_package()
