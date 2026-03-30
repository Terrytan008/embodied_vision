# Embodied Vision — 标定指南

## 标定类型

| 类型 | 时机 | 工具 |
|------|------|------|
| 工厂标定 | 出厂时一次性 | 张正友棋盘格法 + `online_calibrator.py` |
| 在线标定 | 运行时持续 | 驱动内置后台线程 |

## 工厂标定

使用随附的 `online_calibrator.py` 脚本：

```bash
# 采集棋盘格图像对
python3 scripts/online_calibrator.py \
  --mode factory \
  --board-size 9,6 \
  --square-size 25.0 \
  --output ./calibration_data \
  --num-frames 30
```

棋盘格建议：
- 使用 9×6 内角点的标准棋盘格
- 每个方格尺寸 25mm（亚像素精度要求）
- 采集 20~30 对图像，覆盖视角：远/近/左/右/倾斜
- 确保左右目同时看到完整棋盘格

## 在线标定（运行时）

在线标定自动在后台运行，无需人工干预。

**触发条件**：
- 温度变化 > 10°C（机械变形补偿）
- 碰撞后自动触发
- 手动调用服务：

```bash
ros2 service call /stereo_camera_node/recalibrate std_srvs/srv/Trigger
```

**标定质量判断**：

| `recalib_confidence` | 含义 | 动作建议 |
|---------------------|------|---------|
| ≥ 0.90 | 优秀 | 无需操作 |
| 0.80–0.90 | 良好 | 可接受 |
| 0.60–0.80 | 一般 | 建议重新工厂标定 |
| < 0.60 | 差 | 工厂标定，排查硬件 |

**标定参数**：
- `baseline_mm`: 双目基线（机械距离，微调量）
- `left_k / right_k`: 内参矩阵（fx, fy, cx, cy）
- `T_lr`: 左右目外参（旋转+平移向量）

## 标定数据格式

标定结果存储为 JSON 或 ROS2 `StereoCalibrationInfo.msg`：

```json
{
  "left_k": [1194.0, 0.0, 960.0, 0.0, 1194.0, 540.0, 0.0, 0.0, 1.0],
  "right_k": [1194.0, 0.0, 960.0, 0.0, 1194.0, 540.0, 0.0, 0.0, 1.0],
  "baseline_mm": 80.0,
  "recalib_confidence": 0.93,
  "recalib_timestamp_ns": 1711747200000000000
}
```

## 标定质量验证

```bash
# 验证：同一深度物体在深度图上是否各处一致
ros2 run stereo_vision_driver verify_calibration
```

简单自检：用手在相机前 0.5m 处，深度值应在 0.45m~0.55m 范围内，偏差 >10% 说明标定有问题。
