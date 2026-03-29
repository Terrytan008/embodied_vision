#!/bin/bash
# Embodied Vision — 构建脚本
# build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${WS_DIR}/build"

echo "=== Embodied Vision 构建 ==="
echo "工作空间: $WS_DIR"

# 检查ROS2
if ! command -v colcon &> /dev/null; then
    echo "[错误] colcon 未安装"
    echo "安装: sudo apt install python3-colcon-common-extensions"
    exit 1
fi

# 检查CUDA
if ! command -v nvcc &> /dev/null; then
    echo "[警告] CUDA 未安装，GPU加速模块将被禁用"
else
    CUDA_VERSION=$(nvcc --version | grep "release" | awk '{print $5}' | tr -d ',')
    echo "[OK] CUDA $CUDA_VERSION"
fi

# 创建构建目录
mkdir -p "$BUILD_DIR"
cd "$WS_DIR"

# 清理旧构建
echo "[1/3] 清理..."
colcon build --merge-install --cmake-args -DCMAKE_BUILD_TYPE=Release --cmake-args -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tee "${BUILD_DIR}/build.log"

echo ""
echo "[2/3] 构建完成"
echo "构建日志: ${BUILD_DIR}/build.log"

# 运行测试（如果有）
if colcon test --dry-run &> /dev/null; then
    echo "[3/3] 运行测试..."
    colcon test --merge-install --event-handlers console_direct+
    colcon test-result --verbose
else
    echo "[3/3] 跳过测试（无可测试包）"
fi

echo ""
echo "=== 构建完成 ==="
echo "source: source install/setup.bash"
