"""
jetson_side.launch.py

Run on JETSON (192.168.2.2):
  source /opt/ros/humble/setup.bash
  source ~/ros2_ws/install/setup.bash
  ros2 launch auv_camera_bringup jetson_side.launch.py
"""

import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    pkg = get_package_share_directory('auv_camera_bringup')
    config = os.path.join(pkg, 'config', 'camera_jetson.yaml')

    return LaunchDescription([
        # -----------------------------------------------------------------------
        # GStreamer H264/NVENC → UDP streamer
        # -----------------------------------------------------------------------
        Node(
            package    = 'auv_camera_stream',
            executable = 'jetson_streamer_node',
            name       = 'jetson_streamer',
            output     = 'screen',
            parameters = [config],
        ),

        # -----------------------------------------------------------------------
        # V4L2 hardware control bridge
        # (also updates device path from /camera/status topic automatically)
        # -----------------------------------------------------------------------
        Node(
            package    = 'auv_camera_control',
            executable = 'camera_control_node',
            name       = 'camera_control',
            output     = 'screen',
            parameters = [config],
        ),
    ])
