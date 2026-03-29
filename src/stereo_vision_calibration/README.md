# 置信度网络训练

## 快速开始

### 1. 安装依赖

```bash
pip install torch torchvision numpy opencv-python
pip install onnxruntime onnx onnxsim  # 推理用
pip install tensorboard  # 可视化
# GPU 支持 (推荐)
pip install torch --index-url https://download.pytorch.org/whl/cu121
```

### 2. 准备数据集

数据集目录结构：

```
data/confidence_dataset/
├── scene_001/
│   ├── left.png         # 左目图像 (PNG, 任意分辨率)
│   ├── right.png        # 右目图像
│   ├── depth.npy        # 真实深度图 (H×W, float32, 米)
│   └── confidence_gt.npy  # 真实置信度图 (H×W, float32, 0.0~1.0) [可选]
├── scene_002/
│   └── ...
```

**推荐数据集：**
- [SceneFlow (FlyingThings3D)](https://lmb.informatik.uni-freiburg.de/resources/datasets/SceneFlow/)
- [KITTI 2015 Stereo](http://www.cvlibs.net/datasets/kitti/eval_scene_flow.php?benchmark=stereo)
- [Middlebury Stereo v3](https://vision.middlebury.edu/stereo/data/)

**置信度GT生成脚本（用于SceneFlow）：**
```python
# 自动生成置信度GT（基于遮挡mask + 视差一致性）
import numpy as np
import cv2

def generate_confidence_gt(left_img, right_img, disparity, occlusion_mask):
    """
    基于遮挡 + 纹理生成伪置信度GT
    - 有遮挡 → 置信度=0
    - 低纹理区域 → 低置信度
    """
    conf = np.ones_like(disparity, dtype=np.float32)

    # 遮挡区域
    conf[occlusion_mask > 0] = 0.0

    # 低纹理区域（Sobel梯度）
    gray = cv2.cvtColor(left_img, cv2.COLOR_BGR2GRAY)
    sobel = cv2.Sobel(gray, CV_64F, 1, 0, ksize=3)
    texture = np.abs(sobel)
    texture = (texture / texture.max()).astype(np.float32)
    conf = conf * np.clip(texture * 1.5, 0, 1)

    return conf
```

### 3. 训练

```bash
cd src/stereo_vision_calibration

# 训练（使用你自己的数据集路径）
python3 scripts/train_confidence.py \
    --data_root ./data/confidence_dataset \
    --output_dir ./checkpoints \
    --epochs 50 \
    --batch_size 8 \
    --lr 1e-3 \
    --img_h 384 \
    --img_w 1280 \
    --base_channels 32 \
    --pos_weight 2.5 \
    --tensorboard_dir ./runs

# 查看训练进度
tensorboard --logdir ./runs --port 6006
```

### 4. 导出模型

```bash
# 导出 ONNX（CPU/GPU）
python3 scripts/export_onnx.py \
    --checkpoint ./checkpoints/best_f1.pt \
    --output ./models/confidence_net.onnx \
    --img_h 384 --img_w 1280

# 验证模型大小
ls -lh ./models/confidence_net.onnx
# 目标：< 5MB（轻量化）
```

### 5. 集成到 ROS2 驱动

C++ 推理：

```cpp
#include "stereo_vision/hardware/confidence_onnx.hpp"

auto conf_engine = std::make_unique<ConfidenceInference>();
ConfidenceInference::Config cfg;
cfg.model_path = "models/confidence_net.onnx";
cfg.img_height = 384;
cfg.img_width = 1280;
cfg.use_gpu = true;

if (conf_engine->initialize(cfg)) {
    cv::Mat conf = conf_engine->infer(left_gray, right_gray, disparity);
    // conf 现在可以直接用于过滤深度
}
```

## 训练参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--epochs` | 50 | 训练轮数 |
| `--batch_size` | 8 | 批次大小 |
| `--lr` | 1e-3 | 学习率 |
| `--pos_weight` | 2.5 | 正样本（高置信）权重 |
| `--base_channels` | 32 | 基础通道数（影响模型大小）|
| `--img_h/w` | 384/1280 | 输入分辨率 |

## 模型规格

| 项目 | 值 |
|------|-----|
| 输入 | 3×384×1280 (左+右+视差) |
| 输出 | 1×384×1280 (置信度) |
| 参数量 | ~0.8M (base_channels=32) |
| 建议BOM | ~3MB (INT8量化后) |
| 推理延迟 | <10ms (Jetson Orin GPU) |
