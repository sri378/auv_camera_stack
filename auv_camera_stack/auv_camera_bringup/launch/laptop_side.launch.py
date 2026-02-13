"""
laptop_side.launch.py

Run on LOQ LAPTOP (192.168.2.1):
  source /opt/ros/humble/setup.bash
  source ~/ros2_ws/install/setup.bash
  ros2 launch auv_camera_bringup laptop_side.launch.py

Optionally preview the feed:
  ros2 run rqt_image_view rqt_image_view /camera/image_raw
  or:
  gst-launch-1.0 udpsrc port=5600 \\
    ! application/x-rtp,encoding-name=H264,payload=96 \\
    ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! autovideosink
"""

import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    pkg = get_package_share_directory('auv_camera_bringup')
    laptop_config = os.path.join(pkg, 'config', 'camera_laptop.yaml')

    return LaunchDescription([
        # -----------------------------------------------------------------------
        # UDP H264 receiver → publishes /camera/image_raw
        # -----------------------------------------------------------------------
        Node(
            package    = 'auv_camera_receiver',
            executable = 'camera_receiver_node',
            name       = 'camera_receiver',
            output     = 'screen',
            parameters = [laptop_config],
        ),

        # -----------------------------------------------------------------------
        # Adaptive exposure controller
        # Reads /camera/brightness → publishes /camera/control to Jetson
        # -----------------------------------------------------------------------
        Node(
            package    = 'auv_camera_adaptive',
            executable = 'adaptive_controller_node.py',
            name       = 'adaptive_controller',
            output     = 'screen',
            parameters = [laptop_config],
        ),
    ])
