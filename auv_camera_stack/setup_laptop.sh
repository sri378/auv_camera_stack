#!/bin/bash
# setup_laptop.sh
# Run once on Lenovo LOQ to install dependencies and build
# Usage: bash setup_laptop.sh

set -e
echo "======================================================"
echo " AUV Camera Stack — Lenovo LOQ Setup"
echo " Ubuntu 22.04 | ROS2 Humble"
echo "======================================================"

if [ ! -f /opt/ros/humble/setup.bash ]; then
  echo "ERROR: ROS2 Humble not found."
  exit 1
fi
source /opt/ros/humble/setup.bash

echo ""
echo "[1/4] Installing dependencies..."
sudo apt update -q
sudo apt install -y \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-good1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \        # ← provides avdec_h264
  ros-humble-rqt-image-view \
  python3-pip

# Allow UDP port 5600
echo "Opening UDP port 5600..."
sudo ufw allow 5600/udp 2>/dev/null || true

# 2. Verify avdec_h264
echo ""
echo "[2/4] Verifying H264 decode plugin..."
if gst-inspect-1.0 avdec_h264 > /dev/null 2>&1; then
  echo "  ✓ avdec_h264 found"
else
  echo "  ✗ avdec_h264 NOT found — install gstreamer1.0-libav"
fi

# 3. Build
echo ""
echo "[3/4] Building ROS2 packages..."
WORKSPACE="$HOME/ros2_ws"
mkdir -p "$WORKSPACE/src"

STACK_SRC="$WORKSPACE/src/auv_camera_stack"
if [ ! -d "$STACK_SRC" ]; then
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cp -r "$SCRIPT_DIR" "$STACK_SRC"
fi

cd "$WORKSPACE"
colcon build --packages-select auv_camera_msgs
source install/setup.bash

colcon build --packages-select \
  auv_camera_bringup \
  auv_camera_receiver \
  auv_camera_adaptive

source install/setup.bash

echo ""
echo "[4/4] Quick connectivity test (Jetson must be on)..."
if ping -c 1 -W 2 192.168.2.2 > /dev/null 2>&1; then
  echo "  ✓ Jetson 192.168.2.2 reachable"
else
  echo "  ✗ Jetson 192.168.2.2 not reachable — check network"
fi

echo ""
echo "======================================================"
echo " Setup complete!"
echo ""
echo " To run:"
echo "   source /opt/ros/humble/setup.bash"
echo "   source ~/ros2_ws/install/setup.bash"
echo "   export ROS_DOMAIN_ID=42"
echo "   ros2 launch auv_camera_bringup laptop_side.launch.py"
echo ""
echo " To view feed:"
echo "   ros2 run rqt_image_view rqt_image_view /camera/image_raw"
echo "======================================================"
