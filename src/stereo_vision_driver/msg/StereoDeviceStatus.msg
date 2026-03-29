# 设备状态诊断消息
# 发布频率：1Hz

std_msgs/Header header

# 各模块健康状态
bool sensor_left_ok
bool sensor_right_ok
bool imu_ok
bool temp_sensor_ok

# 运行统计
uint32 dropped_frames        # 本周期内丢帧数
uint32 total_frames         # 总帧数
float32 frame_rate          # 当前帧率 (Hz)

# 温度 (摄氏度)
float32 temperature_c

# 最新硬件时间戳 (微秒)
uint64 last_hw_timestamp_ns

# 错误计数
uint32 error_count
string last_error_msg

# 系统状态: 0=正常, 1=警告, 2=错误
uint8 status_level
