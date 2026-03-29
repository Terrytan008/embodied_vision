# Embodied Vision — 驱动启动文件
# driver.launch.py

from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration


def generate_launch_description():

    # ---- 参数声明 ----
    publish_hz = DeclareLaunchArgument(
        'publish_hz', default_value='10',
        description='发布频率 Hz (设为0则启用模拟器模式)'
    )
    confidence_threshold = DeclareLaunchArgument(
        'confidence_threshold', default_value='0.65',
        description='置信度阈值 (0.0~1.0)'
    )
    depth_min = DeclareLaunchArgument(
        'depth_min', default_value='0.1',
        description='最小深度 (米)'
    )
    depth_max = DeclareLaunchArgument(
        'depth_max', default_value='10.0',
        description='最大深度 (米)'
    )
    exposure_mode = DeclareLaunchArgument(
        'exposure_mode', default_value='auto',
        description='曝光模式: auto / manual'
    )
    hdr_mode = DeclareLaunchArgument(
        'hdr_mode', default_value='hdr_x2',
        description='HDR模式: off / hdr_x2 / hdr_x4'
    )
    simulator = DeclareLaunchArgument(
        'simulator', default_value='false',
        description='强制模拟器模式 (用于无硬件开发)'
    )

    # ---- 驱动节点 ----
    camera_node = Node(
        package='stereo_vision_driver',
        executable='stereo_camera_node',
        name='stereo_camera_node',
        parameters=[{
            # simulator=true 时 publish_hz 设为 0 触发模拟器
            'publish_hz': LaunchConfiguration('simulator').evaluate() == 'true'
                          and 10 or LaunchConfiguration('publish_hz'),
            'confidence_threshold': LaunchConfiguration('confidence_threshold'),
            'depth_min_m': LaunchConfiguration('depth_min'),
            'depth_max_m': LaunchConfiguration('depth_max'),
            'exposure_mode': LaunchConfiguration('exposure_mode'),
            'hdr_mode': LaunchConfiguration('hdr_mode'),
        }],
        output='screen',
        emulate_tty=True,
        respawn=True,
        respawn_delay=2.0,
    )

    return LaunchDescription([
        publish_hz,
        confidence_threshold,
        depth_min,
        depth_max,
        exposure_mode,
        hdr_mode,
        simulator,
        camera_node,
    ])
