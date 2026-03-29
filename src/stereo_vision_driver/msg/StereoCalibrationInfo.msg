# 标定状态消息
# 发布频率：1Hz

std_msgs/Header header

# 左目内参矩阵 (3x3 flatten)
float32[9] left_k

# 右目内参矩阵 (3x3 flatten)
float32[9] right_k

# 左右目外参 (RT, 3x4 flatten)
float32[12] T_lr

# 左目畸变系数 (k1, k2, p1, p2, k3)
float32[5] left_d

# 右目畸变系数
float32[5] right_d

# 基线距离 (毫米)
float32 baseline_mm

# 在线标定收敛质量 (0.0~1.0)
float32 calib_quality

# 是否已收敛
bool is_calibrated

# 收敛时间戳 (纳秒)
uint64 calib_timestamp_ns

# 推荐重收敛标志
bool recalibration_recommended
