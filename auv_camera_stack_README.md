# AUV Camera Stack

Jetson Orin Nano (JetPack 6) → Lenovo LOQ ROS2 Humble video stream
for ROV/AUV competition use.

---

## Architecture

```
JETSON (192.168.2.2)                     LOQ (192.168.2.1)
┌─────────────────────────────────┐      ┌─────────────────────────────────┐
│  Groov-e USB Camera             │      │  camera_receiver_node           │
│  /dev/video0 (MJPEG 1920x1080)  │      │  UDP:5600 → avdec_h264          │
│         │                       │      │  → /camera/image_raw (BGR8)     │
│  jetson_streamer_node           │      │         │                       │
│  v4l2src → nvv4l2decoder        │ UDP  │  adaptive_controller_node       │
│  → nvvidconv → nvv4l2h264enc    │─────▶│  /camera/brightness             │
│  → rtph264pay → udpsink         │      │  PI controller + whiteout guard │
│         │                       │ DDS  │         │                       │
│  camera_control_node            │◀─────│  /camera/control →              │
│  V4L2 exposure/WB/gain ctrl     │      │  CameraControl msg              │
└─────────────────────────────────┘      └─────────────────────────────────┘
         ROS_DOMAIN_ID=42 (shared DDS network)
```

**Key design decisions:**
- H264 via `nvv4l2h264enc` (NVENC hardware) — never `vp8enc`/`x264enc`
- Camera found via `/sys/class/video4linux/videoX/name` — not systemd ID_SERIAL
- `io-mode=2` on v4l2src — prevents Jetson memory pool exhaustion
- Auto exposure/WB OFF by default — prevents whiteout from surface reflections
- GStreamer managed via C API (not `system()`) — proper lifecycle & restart
- Adaptive controller runs on LOQ — sends corrections back via ROS2 DDS

---

## Prerequisites

### On BOTH machines

```bash
sudo apt update
sudo apt install -y ros-humble-rclcpp ros-humble-std-msgs \
  ros-humble-sensor-msgs ros-humble-std-srvs \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-good1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
  v4l-utils
```

### On JETSON only

```bash
# Verify NVENC plugins (JetPack 6 provides these)
gst-inspect-1.0 nvv4l2h264enc
gst-inspect-1.0 nvv4l2decoder
gst-inspect-1.0 nvvidconv

# If any are missing:
sudo apt install gstreamer1.0-plugins-nvvideoconvert  # or similar JetPack pkg
```

### On LOQ only

```bash
sudo apt install -y gstreamer1.0-libav    # provides avdec_h264
pip3 install rclpy  # if not already via ros-humble-rclpy
```

---

## Network Configuration

Both machines must be on the same subnet:

```bash
# Jetson
ip addr show  # should show 192.168.2.2

# LOQ
ip addr show  # should show 192.168.2.1

# Test connectivity
ping 192.168.2.1   # from Jetson
ping 192.168.2.2   # from LOQ

# Set same ROS domain on BOTH
export ROS_DOMAIN_ID=42
```

---

## Build

### Clone and build (do this on BOTH machines)

```bash
mkdir -p ~/ros2_ws/src
cd ~/ros2_ws/src
# Copy or git clone the auv_camera_stack folder here

cd ~/ros2_ws

# Build msgs first (both machines need the msg definitions)
colcon build --packages-select auv_camera_msgs
source install/setup.bash

# Build all remaining packages
colcon build --packages-select \
  auv_camera_bringup \
  auv_camera_stream \
  auv_camera_control \
  auv_camera_receiver \
  auv_camera_adaptive
source install/setup.bash
```

---

## Run

### Terminal 1 — ON JETSON (192.168.2.2)

```bash
source /opt/ros/humble/setup.bash
source ~/ros2_ws/install/setup.bash
export ROS_DOMAIN_ID=42

ros2 launch auv_camera_bringup jetson_side.launch.py
```

**What you should see:**
```
[jetson_streamer]: Camera device: /dev/video0
[jetson_streamer]: V4L2 controls applied (manual exposure=200)
[jetson_streamer]: Pipeline:
  v4l2src device=/dev/video0 io-mode=2 ... ! nvv4l2h264enc ... ! udpsink host=192.168.2.1 port=5600
[jetson_streamer]: Pipeline PLAYING → 192.168.2.1:5600
```

### Terminal 2 — ON LOQ (192.168.2.1)

```bash
source /opt/ros/humble/setup.bash
source ~/ros2_ws/install/setup.bash
export ROS_DOMAIN_ID=42

ros2 launch auv_camera_bringup laptop_side.launch.py
```

**What you should see:**
```
[camera_receiver]: RX Pipeline PLAYING on port 5600
[adaptive_controller]: AdaptiveController started (target=100.0)
```

### Terminal 3 — ON LOQ — View feed

```bash
source /opt/ros/humble/setup.bash
ros2 run rqt_image_view rqt_image_view /camera/image_raw
```

Or via GStreamer directly (lightweight preview, no ROS):
```bash
gst-launch-1.0 udpsrc port=5600 \
  ! application/x-rtp,encoding-name=H264,payload=96 \
  ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! autovideosink
```

---

## Manual Control Commands

### Apply safe underwater defaults immediately

```bash
ros2 service call /camera/safe_defaults std_srvs/srv/Trigger
```

### Manually adjust a single control

```bash
# Force a specific exposure (e.g. dimmer water)
ros2 topic pub --once /camera/control auv_camera_msgs/msg/CameraControl \
  "{auto_exposure: false, exposure_time: 150,
    white_balance_auto: false, white_balance_temp: 5500,
    brightness: 0, contrast: 42, saturation: 80,
    hue: 6, gamma: 100, gain: 15, sharpness: 3,
    backlight_comp: 0, power_line_freq: 1}"
```

### Restart pipeline if frozen

```bash
ros2 service call /camera/restart std_srvs/srv/Trigger
```

### Disable adaptive control (use fixed manual settings)

```bash
ros2 param set /adaptive_controller enabled false
```

### Check status

```bash
ros2 topic echo /camera/status
ros2 topic hz /camera/image_raw    # Should show ~25 Hz
```

---

## Tuning for Competition Conditions

### Harsh outdoor light (2PM)

```yaml
# In camera_laptop.yaml
target_brightness:  80.0      # Lower target = darker image
max_exposure:      400         # Tighter ceiling
whiteout_threshold: 190.0     # More aggressive whiteout detection
```

### Dim / late afternoon (5-6PM)

```yaml
target_brightness:  110.0
min_exposure:       100
max_exposure:      1200
```

### Underwater (reduced light)

```yaml
target_brightness:  120.0
min_exposure:       200
max_exposure:      2000
default_gain:        25        # in camera_jetson.yaml
default_saturation:  90        # boost underwater color absorption
```

---

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| Camera not found | USB hub overloaded | Direct USB only, no hubs |
| Camera not found | systemd ID_SERIAL missing | ✅ Fixed: uses sysfs name match |
| Pipeline crashes instantly | nvv4l2h264enc not found | `gst-inspect-1.0 nvv4l2h264enc` — reinstall JetPack GStreamer |
| White screen | Auto exposure runaway | Call `/camera/safe_defaults` service |
| `not-negotiated (-4)` | io-mode or caps mismatch | ✅ Fixed: explicit MJPEG caps + io-mode=2 |
| No image on LOQ | UDP blocked | `sudo ufw allow 5600/udp` on LOQ |
| Choppy video | Too high bitrate for USB bandwidth | Lower `bitrate_kbps` to 2000 |
| High latency | Large jitter buffer | Already set: `sync=false`, `max-buffers=2`, `drop=true` |

---

## Competition Day Checklist

```
□ Jetson powered via official PSU (not USB power bank)
□ Camera connected DIRECTLY to Jetson USB (no hubs)
□ Network: Jetson=192.168.2.2, LOQ=192.168.2.1, ping OK both ways
□ ROS_DOMAIN_ID=42 set on both terminals
□ colcon build + source install/setup.bash done on both
□ gst-inspect-1.0 nvv4l2h264enc shows result (Jetson)
□ gst-inspect-1.0 avdec_h264 shows result (LOQ)
□ ros2 service call /camera/safe_defaults → verify feed looks good
□ ros2 topic hz /camera/image_raw → shows ~25 Hz
□ rqt_image_view feed: sharp, correct color, not white
□ Test adaptive: cover camera with hand → uncover → watch recovery
□ Measure latency: ros2 topic delay /camera/image_raw (should be <200ms)
```
