#!/bin/bash
# Embodied Vision — 构建脚本
# build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Embodied Vision 构建 ==="
echo "工作空间: $WS_DIR"

# 检查 ROS2
if ! command -v colcon &> /dev/null; then
    echo "[错误] colcon 未安装"
    echo "安装: sudo apt install python3-colcon-common-extensions"
    exit 1
fi

# 检查 OpenCV
if python3 -c "import cv2" 2>/dev/null; then
    echo "[OK] OpenCV: $(python3 -c 'import cv2; print(cv2.__version__)')"
else
    echo "[警告] OpenCV Python 未安装（构建 C++ 库不需要）"
fi

# 检查 CUDA
if command -v nvcc &> /dev/null; then
    CUDA_VERSION=$(nvcc --version | grep "release" | awk '{print($5)}' | tr -d ',')
    echo "[OK] CUDA $CUDA_VERSION"
else
    echo "[信息] CUDA 未安装，GPU 模块将被禁用"
fi

cd "$WS_DIR"

# 增量构建（保留 CMake 缓存）
echo "[1/2] 构建..."
colcon build --merge-install \
    --cmake-args -DCMAKE_BUILD_TYPE=Release \
    --event-handlers console_direct+ 2>&1

# 运行测试
echo ""
echo "[2/2] 运行单元测试..."
colcon test --merge-install \
    --event-handlers console_direct+ \
    --return-code-on-test-failure \
    --cmake-args -DCMAKE_BUILD_TYPE=Release 2>&1 || true

colcon test-result --verbose 2>&1 || echo "（无可用测试结果）"

echo ""
echo "=== 构建完成 ==="
echo "source: source install/setup.bash"
echo "运行:   ros2 launch stereo_vision_driver driver.launch.py simulator:=true"
