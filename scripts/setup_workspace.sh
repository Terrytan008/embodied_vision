#!/bin/bash
# Embodied Vision — 工作空间初始化
# 运行一次即可（或每次打开新终端自动 source）

# === 1. 加载 ROS2 ===
if [ -f /opt/ros/humble/setup.bash ]; then
    source /opt/ros/humble/setup.bash
elif [ -f /opt/ros/jazzy/setup.bash ]; then
    source /opt/ros/jazzy/setup.bash
else
    echo "[错误] 未找到 ROS2 (humble/jazzy)"
    echo "安装: https://docs.ros.org/en/humble/Installation.html"
    return 1 2>/dev/null || exit 1
fi

# === 2. 加载本工作空间 ===
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_SETUP="${REPO_DIR}/install/setup.bash"

if [ -f "$WORKSPACE_SETUP" ]; then
    source "$WORKSPACE_SETUP"
    echo "[OK] Embodied Vision 工作空间已加载"
else
    echo "[警告] 工作空间未构建，先运行:"
    echo "  cd $REPO_DIR && ./scripts/build.sh"
fi

# === 3. 可选：配置 NVIDIA DRIVE OS 或 Jetson 平台 ===
if [ -f /etc/aarch64-linux-gnu/tegra env vars ]; then
    echo "[信息] NVIDIA Jetson/DRIVE 环境"
fi

# === 4. 便捷别名 ===
alias ev-build='cd '"$REPO_DIR"' && colcon build --merge-install && source install/setup.bash'
alias ev-sim='ros2 launch stereo_vision_driver driver.launch.py simulator:=true'
alias ev-status='ros2 node list && ros2 topic list'

echo ""
echo "=== 快速命令 ==="
echo "  ev-build   # 构建并刷新环境"
echo "  ev-sim     # 启动模拟器模式"
echo "  ev-status  # 查看运行中节点/话题"
