#!/bin/bash
# setup_jetson.sh
# Run once on Jetson Orin Nano to install dependencies and build
# Usage: bash setup_jetson.sh

set -e
echo "======================================================"
echo " AUV Camera Stack — Jetson Orin Nano Setup"
echo " JetPack 6 | Ubuntu 22.04 | ROS2 Humble"
echo "======================================================"

# 1. ROS2 Humble environment
if [ ! -f /opt/ros/humble/setup.bash ]; then
  echo "ERROR: ROS2 Humble not found. Install it first."
  exit 1
fi
source /opt/ros/humble/setup.bash

# 2. System dependencies
echo ""
echo "[1/5] Installing system dependencies..."
sudo apt update -q
sudo apt install -y \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-good1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  v4l-utils \
  python3-pip

# 3. Verify NVENC plugins (JetPack provides these)
echo ""
echo "[2/5] Verifying NVENC GStreamer plugins..."
MISSING=0
for plugin in nvv4l2h264enc nvv4l2decoder nvvidconv; do
  if gst-inspect-1.0 "$plugin" > /dev/null 2>&1; then
    echo "  ✓ $plugin found"
  else
    echo "  ✗ $plugin NOT found — check JetPack GStreamer installation"
    MISSING=1
  fi
done

if [ "$MISSING" -eq 1 ]; then
  echo ""
  echo "WARNING: Some NVENC plugins missing."
  echo "On JetPack 6, try:"
  echo "  sudo apt install nvidia-l4t-gstreamer"
  echo "  sudo apt install gstreamer1.0-plugins-nvvideoconvert"
fi

# 4. Build workspace
echo ""
echo "[3/5] Setting up ROS2 workspace..."
WORKSPACE="$HOME/ros2_ws"
mkdir -p "$WORKSPACE/src"

# Check if stack is already in src
STACK_SRC="$WORKSPACE/src/auv_camera_stack"
if [ ! -d "$STACK_SRC" ]; then
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  echo "Copying stack from $SCRIPT_DIR to $STACK_SRC"
  cp -r "$SCRIPT_DIR" "$STACK_SRC"
fi

echo ""
echo "[4/5] Building packages..."
cd "$WORKSPACE"

# Build msgs first (needed by other packages)
colcon build --packages-select auv_camera_msgs
source install/setup.bash

# Build Jetson-specific packages
colcon build --packages-select \
  auv_camera_bringup \
  auv_camera_stream \
  auv_camera_control

source install/setup.bash

echo ""
echo "[5/5] Verifying camera detection..."
if ls /dev/video* 2>/dev/null; then
  echo "  Video devices found:"
  for d in /dev/video*; do
    NAME=$(cat "/sys/class/video4linux/$(basename $d)/name" 2>/dev/null || echo "unknown")
    echo "    $d → $NAME"
  done
else
  echo "  No /dev/video* found — connect camera and re-run"
fi

echo ""
echo "======================================================"
echo " Setup complete!"
echo ""
echo " To run:"
echo "   source /opt/ros/humble/setup.bash"
echo "   source ~/ros2_ws/install/setup.bash"
echo "   export ROS_DOMAIN_ID=42"
echo "   ros2 launch auv_camera_bringup jetson_side.launch.py"
echo "======================================================"
