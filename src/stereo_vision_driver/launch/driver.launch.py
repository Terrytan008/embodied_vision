# Embodied Vision — 驱动启动文件
# driver.launch.py

from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration


def _launch_setup(context, *args):
    simulator_val = LaunchConfiguration('simulator').perform(context)
    hz = 0 if simulator_val == 'true' else int(LaunchConfiguration('publish_hz').perform(context))

    return [Node(
        package='stereo_vision_driver',
        executable='stereo_camera_node',
        name='stereo_camera_node',
        parameters=[{
            'publish_hz': hz,
            'confidence_threshold': float(LaunchConfiguration('confidence_threshold').perform(context)),
            'depth_min_m': float(LaunchConfiguration('depth_min').perform(context)),
            'depth_max_m': float(LaunchConfiguration('depth_max').perform(context)),
            'exposure_mode': LaunchConfiguration('exposure_mode').perform(context),
            'hdr_mode': LaunchConfiguration('hdr_mode').perform(context),
            'onnx_model_path': '/config/confidence_model.onnx',
        }],
        output='screen',
        emulate_tty=True,
        respawn=True,
        respawn_delay=2.0,
    )]


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

    return LaunchDescription([
        publish_hz,
        confidence_threshold,
        depth_min,
        depth_max,
        exposure_mode,
        hdr_mode,
        simulator,
        OpaqueFunction(function=_launch_setup),
    ])
