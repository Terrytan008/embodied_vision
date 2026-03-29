# 标定工具

## 使用方法

### 工厂标定

```bash
python3 scripts/online_calibrator.py factory \
  --left-dir ./data/left \
  --right-dir ./data/right \
  --output calibration.json \
  --board-size 9 6 \
  --square-size 25.0
```

### 标定质量评估

```bash
python3 scripts/online_calibrator.py evaluate \
  --params calibration.json \
  --left-dir ./test/left \
  --right-dir ./test/right
```

## 数据集目录结构

```
data/
├── left/
│   ├── IMG_0001.png
│   └── IMG_0002.png
└── right/
    ├── IMG_0001.png
    └── IMG_0002.png
```

## 标定板要求

- 使用 9×6 内角点棋盘格
- 棋盘格尺寸：25mm×25mm（可调整）
- 采集 20-30 组不同角度的图像
- 确保左右图同步采集（时间差<50ms）
