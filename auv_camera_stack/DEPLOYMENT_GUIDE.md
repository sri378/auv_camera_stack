# Step-by-Step: Push → Build → Run → Live Stream

---

## PHASE 0 — Push to GitHub (do this on your LOQ first)

### Step 1: Create a new GitHub repo

1. Go to https://github.com/new
2. Name it: `auv_camera_stack`
3. Set to **Private**
4. **Do NOT** tick "Add README" or .gitignore
5. Click **Create repository**
6. Copy the repo URL — looks like: `https://github.com/YOUR_USERNAME/auv_camera_stack.git`

---

### Step 2: Push the code from your LOQ

Open a terminal on your LOQ. Unzip the downloaded file first:

```bash
cd ~/Downloads
unzip auv_camera_stack.zip
cd auv_camera_stack
```

Now initialize git and push:

```bash
git init
git add .
git commit -m "Initial AUV camera stack commit"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/auv_camera_stack.git
git push -u origin main
```

> **If it asks for password:** GitHub no longer accepts passwords.
> You need a Personal Access Token (PAT).
> Go to: GitHub → Settings → Developer settings → Personal access tokens → Tokens (classic) → Generate new token
> Tick: `repo` scope → Generate → copy the token → paste it as your password.

Verify it worked:
```bash
# You should see your files at:
# https://github.com/YOUR_USERNAME/auv_camera_stack
```

---

## PHASE 1 — Set Up Jetson Orin Nano (192.168.2.2)

Connect a keyboard, monitor, and Ethernet to your Jetson.
Open a terminal on the Jetson.

---

### Step 3: Install ROS2 Humble on Jetson (skip if already installed)

```bash
# Check if already installed
ros2 --version
# If you see "ros2 cli version X.X.X" → skip to Step 4
```

If NOT installed:
```bash
sudo apt update && sudo apt install -y software-properties-common
sudo add-apt-repository universe
sudo apt update && sudo apt install -y curl
curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
  -o /usr/share/keyrings/ros-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] \
  http://packages.ros.org/ros2/ubuntu $(. /etc/os-release && echo $UBUNTU_CODENAME) main" \
  | sudo tee /etc/apt/sources.list.d/ros2.list
sudo apt update
sudo apt install -y ros-humble-desktop python3-colcon-common-extensions
echo "source /opt/ros/humble/setup.bash" >> ~/.bashrc
source ~/.bashrc
```

---

### Step 4: Install GStreamer + dependencies on Jetson

```bash
sudo apt update
sudo apt install -y \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-good1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  v4l-utils \
  git
```

---

### Step 5: Verify NVENC plugins (CRITICAL for Jetson)

```bash
gst-inspect-1.0 nvv4l2h264enc
gst-inspect-1.0 nvv4l2decoder
gst-inspect-1.0 nvvidconv
```

Each command should print a long description of the plugin.
If you see `No such element or plugin` for any of them:

```bash
# Fix: reinstall JetPack GStreamer packages
sudo apt install -y nvidia-l4t-gstreamer
# Then re-run the gst-inspect-1.0 checks above
```

---

### Step 6: Create ROS2 workspace and clone repo on Jetson

```bash
mkdir -p ~/ros2_ws/src
cd ~/ros2_ws/src
git clone https://github.com/YOUR_USERNAME/auv_camera_stack.git
ls  # You should see: auv_camera_stack/
```

---

### Step 7: Build on Jetson

```bash
cd ~/ros2_ws

# Source ROS2
source /opt/ros/humble/setup.bash

# Step 7a: Build messages FIRST (other packages depend on this)
colcon build --packages-select auv_camera_msgs
source install/setup.bash

# Step 7b: Build Jetson packages
colcon build --packages-select \
  auv_camera_bringup \
  auv_camera_stream \
  auv_camera_control

# Step 7c: Source the workspace
source install/setup.bash
```

Expected output:
```
Starting >>> auv_camera_stream
Finished <<< auv_camera_stream [XX.Xs]
Starting >>> auv_camera_control
Finished <<< auv_camera_control [XX.Xs]

Summary: X packages finished
```

> If you get errors mentioning `gst/gst.h not found`:
> ```bash
> sudo apt install -y libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
> colcon build --packages-select auv_camera_stream auv_camera_control
> ```

---

### Step 8: Add permanent environment to Jetson .bashrc

```bash
echo "source /opt/ros/humble/setup.bash" >> ~/.bashrc
echo "source ~/ros2_ws/install/setup.bash" >> ~/.bashrc
echo "export ROS_DOMAIN_ID=42" >> ~/.bashrc
source ~/.bashrc
```

---

## PHASE 2 — Set Up Lenovo LOQ (192.168.2.1)

Open a terminal on your LOQ.

---

### Step 9: Install GStreamer + dependencies on LOQ

```bash
sudo apt update
sudo apt install -y \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  ros-humble-rqt-image-view \
  git
```

> `gstreamer1.0-libav` is critical — it provides `avdec_h264` which decodes the H264 stream.

Verify:
```bash
gst-inspect-1.0 avdec_h264
# Should print plugin description, not an error
```

---

### Step 10: Allow UDP port 5600 through firewall on LOQ

```bash
sudo ufw allow 5600/udp
sudo ufw status  # Should show 5600/udp ALLOW
```

---

### Step 11: Create workspace and clone repo on LOQ

```bash
mkdir -p ~/ros2_ws/src
cd ~/ros2_ws/src
git clone https://github.com/YOUR_USERNAME/auv_camera_stack.git
```

---

### Step 12: Build on LOQ

```bash
cd ~/ros2_ws
source /opt/ros/humble/setup.bash

# Step 12a: Build messages first
colcon build --packages-select auv_camera_msgs
source install/setup.bash

# Step 12b: Build LOQ packages
colcon build --packages-select \
  auv_camera_bringup \
  auv_camera_receiver \
  auv_camera_adaptive

source install/setup.bash
```

---

### Step 13: Add permanent environment to LOQ .bashrc

```bash
echo "source /opt/ros/humble/setup.bash" >> ~/.bashrc
echo "source ~/ros2_ws/install/setup.bash" >> ~/.bashrc
echo "export ROS_DOMAIN_ID=42" >> ~/.bashrc
source ~/.bashrc
```

---

## PHASE 3 — Network Check (Before Running Anything)

Make sure both machines are connected via Ethernet (direct cable or through a switch).

### Step 14: Verify IPs and ping

On Jetson:
```bash
ip addr show
# Look for: 192.168.2.2
ping -c 3 192.168.2.1   # Ping LOQ from Jetson
```

On LOQ:
```bash
ip addr show
# Look for: 192.168.2.1
ping -c 3 192.168.2.2   # Ping Jetson from LOQ
```

Both pings must succeed before proceeding.

> If IP is wrong, set it manually:
> ```bash
> # On Jetson
> sudo ip addr add 192.168.2.2/24 dev eth0
> # On LOQ
> sudo ip addr add 192.168.2.1/24 dev eth0
> ```

---

### Step 15: Verify camera is detected on Jetson

Plug in your Groov-e USB camera directly to Jetson (NO hub):

```bash
# Check it appears in lsusb
lsusb | grep -i groov
# Should show: Bus 00X Device 00X: ID XXXX:XXXX Groov-e USB Camera

# Check video device
ls /dev/video*
# Should show: /dev/video0 (possibly video1, video2)

# Check the name via sysfs (this is how our code finds it)
cat /sys/class/video4linux/video0/name
# Should show: Groov-e USB Camera: Groov-e USB

# Full camera info
v4l2-ctl --all -d /dev/video0
# Should show 1920x1080 MJPEG capabilities
```

---

## PHASE 4 — Run It

Open **4 terminals** total — 2 on Jetson, 2 on LOQ.

---

### TERMINAL 1 — Jetson: Start the stream

```bash
source ~/.bashrc   # loads ROS2 + workspace + DOMAIN_ID
ros2 launch auv_camera_bringup jetson_side.launch.py
```

**Wait for this output before touching LOQ:**
```
[jetson_streamer]: Camera device: /dev/video0
[jetson_streamer]: V4L2 controls applied (manual exposure=200)
[jetson_streamer]: Pipeline PLAYING → 192.168.2.1:5600
```

If you see `ERROR: Camera 'Groov-e' not found` → camera not detected, check Step 15.

---

### TERMINAL 2 — LOQ: Start the receiver

```bash
source ~/.bashrc
ros2 launch auv_camera_bringup laptop_side.launch.py
```

**Expected output:**
```
[camera_receiver]: RX Pipeline PLAYING on port 5600
[adaptive_controller]: AdaptiveController started (target=100.0)
```

---

### TERMINAL 3 — LOQ: View the live feed

```bash
source ~/.bashrc
ros2 run rqt_image_view rqt_image_view /camera/image_raw
```

A window opens. Select `/camera/image_raw` from the dropdown if not auto-selected.
You should see your live camera feed within 2–3 seconds.

**Alternative lightweight preview (no ROS, pure GStreamer):**
```bash
gst-launch-1.0 udpsrc port=5600 \
  ! "application/x-rtp,encoding-name=H264,payload=96" \
  ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! autovideosink
```

---

### TERMINAL 4 — LOQ: Monitor status

```bash
source ~/.bashrc

# Watch camera status (updates every 1 second)
ros2 topic echo /camera/status

# Check that image is arriving at ~25 Hz
ros2 topic hz /camera/image_raw

# Watch brightness value (used by adaptive controller)
ros2 topic echo /camera/brightness
```

---

## PHASE 5 — Testing

### Test 1: Whiteout protection

Point the camera at a bright light or white surface.
Watch TERMINAL 2 — you should see:
```
[adaptive_controller]: ⚠ WHITEOUT detected (brightness=215.3)! Emergency exposure cut → 30
```
The image should recover within 1–2 seconds as the adaptive controller cuts exposure.

---

### Test 2: Manual emergency reset

If the image looks wrong at any time:
```bash
# Apply safe underwater defaults instantly
ros2 service call /camera/safe_defaults std_srvs/srv/Trigger
```

---

### Test 3: Force a specific exposure manually

```bash
ros2 topic pub --once /camera/control auv_camera_msgs/msg/CameraControl \
  "{auto_exposure: false, exposure_time: 150,
    white_balance_auto: false, white_balance_temp: 5500,
    brightness: 0, contrast: 42, saturation: 80,
    hue: 6, gamma: 100, gain: 15, sharpness: 3,
    backlight_comp: 0, power_line_freq: 1}"
```

---

### Test 4: Restart pipeline without rebooting

```bash
ros2 service call /camera/restart std_srvs/srv/Trigger
```
The pipeline stops and restarts automatically. Feed should return within 3 seconds.

---

### Test 5: Latency measurement

```bash
# Install ros2-topic-delay if not present
sudo apt install -y ros-humble-ros2topic

# Measure end-to-end image latency
ros2 topic delay /camera/image_raw
# Target: < 200ms is good for ROV control
```

---

## Quick Troubleshooting Reference

| What you see | Fix |
|---|---|
| `Camera 'Groov-e' not found` | Remove USB hub, plug directly. Run `lsusb` to verify. |
| `No such element: nvv4l2h264enc` | `sudo apt install nvidia-l4t-gstreamer` on Jetson |
| `No such element: avdec_h264` | `sudo apt install gstreamer1.0-libav` on LOQ |
| Pipeline starts then immediately stops | Check `ros2 topic echo /camera/status` for error_msg |
| UDP packets not arriving on LOQ | `sudo ufw allow 5600/udp` on LOQ. Verify with `nc -ul 5600` |
| White screen | `ros2 service call /camera/safe_defaults std_srvs/srv/Trigger` |
| `ROS_DOMAIN_ID` mismatch | Both machines must have `export ROS_DOMAIN_ID=42` |
| `colcon build` fails with missing auv_camera_msgs | Build msgs first: `colcon build --packages-select auv_camera_msgs` |

---

## Update code later (after git push)

When you make changes and push to GitHub:

```bash
# On BOTH machines:
cd ~/ros2_ws/src/auv_camera_stack
git pull

# On Jetson:
cd ~/ros2_ws
colcon build --packages-select auv_camera_stream auv_camera_control
source install/setup.bash

# On LOQ:
cd ~/ros2_ws
colcon build --packages-select auv_camera_receiver auv_camera_adaptive
source install/setup.bash
```
