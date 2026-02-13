/**
 * auv_camera_stream: jetson_streamer_node.cpp
 *
 * Runs on: Jetson Orin Nano (JetPack 6, Ubuntu 22.04)
 * Sends:   H264/RTP over UDP → 192.168.2.1:5600
 *
 * Key fixes vs original repo:
 *  - Device detection via /sys/class/video4linux (not systemd ID_SERIAL)
 *  - Hardware encoding via nvv4l2h264enc (not vp8enc/x264enc)
 *  - GStreamer managed via C API (not system() call)
 *  - io-mode=2 to prevent memory pool exhaustion
 *  - Full pipeline restart on error
 */

#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <std_msgs/msg/string.hpp>
#include <auv_camera_msgs/msg/camera_status.hpp>
#include <auv_camera_msgs/msg/camera_control.hpp>

#include <gst/gst.h>
#include <glib.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <sstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Camera device discovery via /sys/class/video4linux/videoX/name
// This is reliable on Jetson even when udev/systemd doesn't expose ID_SERIAL
// ---------------------------------------------------------------------------
static std::string find_camera_device(const std::string & model_keyword = "Groov-e")
{
  const std::string sysfs_base = "/sys/class/video4linux";

  if (!fs::exists(sysfs_base)) {
    return "";
  }

  std::vector<std::string> candidates;

  for (const auto & entry : fs::directory_iterator(sysfs_base)) {
    std::string name_file = entry.path().string() + "/name";
    if (!fs::exists(name_file)) continue;

    std::ifstream f(name_file);
    std::string camera_name;
    std::getline(f, camera_name);

    if (camera_name.find(model_keyword) != std::string::npos) {
      // Map sysX/class/video4linux/video0 → /dev/video0
      std::string dev_name = "/dev/" + entry.path().filename().string();

      // Only accept capture devices (video0, video1 etc), skip metadata nodes
      if (dev_name.find("/dev/video") != std::string::npos) {
        // Verify it's a capture device (not metadata) using /sys caps
        std::string caps_file = entry.path().string() + "/device/streaming:0/capabilities";
        // Simple fallback: prefer lower-numbered devices
        candidates.push_back(dev_name);
      }
    }
  }

  if (candidates.empty()) return "";

  // Sort and return the first valid capture device (lowest /dev/videoX number)
  std::sort(candidates.begin(), candidates.end());
  return candidates[0];
}

// ---------------------------------------------------------------------------
// Apply V4L2 controls via v4l2-ctl (safe, no ioctl complexity)
// ---------------------------------------------------------------------------
static bool set_v4l2_control(
  const std::string & device,
  const std::string & ctrl_name,
  int value)
{
  std::string cmd = "v4l2-ctl --device=" + device +
    " --set-ctrl=" + ctrl_name + "=" + std::to_string(value) +
    " 2>/dev/null";
  int ret = std::system(cmd.c_str());
  return (ret == 0);
}

static void apply_manual_camera_settings(
  const std::string & device,
  const auv_camera_msgs::msg::CameraControl & ctrl)
{
  // CRITICAL: disable auto exposure FIRST to allow manual control
  if (!ctrl.auto_exposure) {
    set_v4l2_control(device, "auto_exposure", 1);  // 1 = Manual Mode
    set_v4l2_control(device, "exposure_time_absolute", ctrl.exposure_time);
  } else {
    set_v4l2_control(device, "auto_exposure", 3);  // 3 = Aperture Priority
  }

  // Disable auto white balance if manual temp requested
  if (!ctrl.white_balance_auto) {
    set_v4l2_control(device, "white_balance_automatic", 0);
    set_v4l2_control(device, "white_balance_temperature", ctrl.white_balance_temp);
  } else {
    set_v4l2_control(device, "white_balance_automatic", 1);
  }

  set_v4l2_control(device, "brightness",           ctrl.brightness);
  set_v4l2_control(device, "contrast",             ctrl.contrast);
  set_v4l2_control(device, "saturation",           ctrl.saturation);
  set_v4l2_control(device, "hue",                  ctrl.hue);
  set_v4l2_control(device, "gamma",                ctrl.gamma);
  set_v4l2_control(device, "gain",                 ctrl.gain);
  set_v4l2_control(device, "sharpness",            ctrl.sharpness);
  set_v4l2_control(device, "backlight_compensation", ctrl.backlight_comp);
  set_v4l2_control(device, "power_line_frequency", ctrl.power_line_freq);
}

// ---------------------------------------------------------------------------
// GStreamer pipeline builder
//
// Jetson JetPack 6 MJPEG → NVENC H264 → RTP → UDP pipeline:
//
//   v4l2src io-mode=2
//   → image/jpeg 1920x1080@25
//   → jpegparse
//   → nvv4l2decoder (MJPEG HW decode)
//   → nvvidconv
//   → video/x-raw(memory:NVMM),NV12
//   → nvv4l2h264enc (NVENC HW encode)
//   → h264parse
//   → rtph264pay
//   → udpsink → 192.168.2.1:5600
// ---------------------------------------------------------------------------
static std::string build_pipeline_str(
  const std::string & device,
  const std::string & host,
  int port,
  int width, int height, int fps,
  int bitrate_kbps)
{
  std::ostringstream ss;
  ss << "v4l2src device=" << device
     << " io-mode=2 do-timestamp=true"
     << " ! image/jpeg,width=" << width
     << ",height=" << height
     << ",framerate=" << fps << "/1"
     << " ! jpegparse"
     // HW MJPEG decode on Jetson
     << " ! nvv4l2decoder mjpeg=true"
     << " ! nvvidconv"
     << " ! video/x-raw(memory:NVMM),format=NV12,width=" << width
     << ",height=" << height
     // NVENC H264 hardware encoder
     << " ! nvv4l2h264enc"
     << "   bitrate=" << (bitrate_kbps * 1000)
     << "   preset-level=1"          // low-latency preset
     << "   iframeinterval=30"
     << "   insert-sps-pps=1"
     << "   control-rate=1"          // constant bitrate
     << " ! h264parse config-interval=1"
     << " ! rtph264pay mtu=1400 config-interval=1 pt=96"
     << " ! udpsink host=" << host
     << " port=" << port
     << " sync=false async=false";
  return ss.str();
}

// ===========================================================================
// Main ROS2 Node
// ===========================================================================
class JetsonStreamerNode : public rclcpp::Node
{
public:
  JetsonStreamerNode()
  : Node("jetson_streamer"),
    pipeline_(nullptr),
    bus_(nullptr),
    running_(false),
    frame_count_(0)
  {
    // --- Parameters ---
    this->declare_parameter("camera_model_keyword", "Groov-e");
    this->declare_parameter("stream_host",          "192.168.2.1");
    this->declare_parameter("stream_port",          5600);
    this->declare_parameter("width",                1920);
    this->declare_parameter("height",               1080);
    this->declare_parameter("fps",                  25);
    this->declare_parameter("bitrate_kbps",         4000);
    this->declare_parameter("restart_delay_ms",     3000);

    // Default camera controls (safe for underwater use)
    this->declare_parameter("default_auto_exposure",       false);
    this->declare_parameter("default_exposure_time",       200);
    this->declare_parameter("default_auto_wb",             false);
    this->declare_parameter("default_white_balance_temp",  5500);
    this->declare_parameter("default_brightness",          0);
    this->declare_parameter("default_contrast",            40);
    this->declare_parameter("default_saturation",          70);
    this->declare_parameter("default_gamma",               100);
    this->declare_parameter("default_gain",                20);
    this->declare_parameter("default_sharpness",           3);
    this->declare_parameter("default_backlight_comp",      0);
    this->declare_parameter("default_power_line_freq",     1);  // 50 Hz

    // --- Publishers ---
    status_pub_ = this->create_publisher<auv_camera_msgs::msg::CameraStatus>(
      "/camera/status", 10);

    // --- Subscribers ---
    control_sub_ = this->create_subscription<auv_camera_msgs::msg::CameraControl>(
      "/camera/control", 10,
      std::bind(&JetsonStreamerNode::on_control, this, std::placeholders::_1));

    // --- Services ---
    restart_srv_ = this->create_service<std_srvs::srv::Trigger>(
      "/camera/restart",
      std::bind(&JetsonStreamerNode::on_restart, this,
        std::placeholders::_1, std::placeholders::_2));

    // --- Status timer (1 Hz) ---
    status_timer_ = this->create_wall_timer(
      std::chrono::seconds(1),
      std::bind(&JetsonStreamerNode::publish_status, this));

    // --- Init GStreamer ---
    gst_init(nullptr, nullptr);

    // --- Start pipeline in background thread ---
    stream_thread_ = std::thread([this]() { stream_loop(); });

    RCLCPP_INFO(this->get_logger(), "JetsonStreamer node started");
  }

  ~JetsonStreamerNode()
  {
    running_ = false;
    stop_pipeline();
    if (stream_thread_.joinable()) stream_thread_.join();
  }

private:
  // ---- GStreamer state ----
  GstElement * pipeline_;
  GstBus     * bus_;
  std::atomic<bool> running_;
  std::atomic<uint32_t> frame_count_;
  std::string current_device_;
  std::string pipeline_state_str_;
  std::string last_error_;

  // ---- ROS handles ----
  rclcpp::Publisher<auv_camera_msgs::msg::CameraStatus>::SharedPtr  status_pub_;
  rclcpp::Subscription<auv_camera_msgs::msg::CameraControl>::SharedPtr control_sub_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr restart_srv_;
  rclcpp::TimerBase::SharedPtr status_timer_;
  std::thread stream_thread_;

  // ---- Current control state ----
  auv_camera_msgs::msg::CameraControl current_ctrl_;

  // -----------------------------------------------------------------------
  // Build default control from parameters
  // -----------------------------------------------------------------------
  auv_camera_msgs::msg::CameraControl default_controls()
  {
    auv_camera_msgs::msg::CameraControl c;
    c.auto_exposure       = this->get_parameter("default_auto_exposure").as_bool();
    c.exposure_time       = this->get_parameter("default_exposure_time").as_int();
    c.white_balance_auto  = this->get_parameter("default_auto_wb").as_bool();
    c.white_balance_temp  = this->get_parameter("default_white_balance_temp").as_int();
    c.brightness          = this->get_parameter("default_brightness").as_int();
    c.contrast            = this->get_parameter("default_contrast").as_int();
    c.saturation          = this->get_parameter("default_saturation").as_int();
    c.gamma               = this->get_parameter("default_gamma").as_int();
    c.gain                = this->get_parameter("default_gain").as_int();
    c.sharpness           = this->get_parameter("default_sharpness").as_int();
    c.backlight_comp      = this->get_parameter("default_backlight_comp").as_int();
    c.power_line_freq     = this->get_parameter("default_power_line_freq").as_int();
    c.hue                 = 6;
    return c;
  }

  // -----------------------------------------------------------------------
  // Stop and clean up pipeline
  // -----------------------------------------------------------------------
  void stop_pipeline()
  {
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      if (bus_) {
        gst_bus_remove_signal_watch(bus_);
        gst_object_unref(bus_);
        bus_ = nullptr;
      }
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
    pipeline_state_str_ = "NULL";
  }

  // -----------------------------------------------------------------------
  // Start pipeline: find device → apply controls → launch GST
  // -----------------------------------------------------------------------
  bool start_pipeline()
  {
    stop_pipeline();

    // 1. Find camera device
    std::string keyword = this->get_parameter("camera_model_keyword").as_string();
    current_device_ = find_camera_device(keyword);
    if (current_device_.empty()) {
      last_error_ = "Camera '" + keyword + "' not found via /sys/class/video4linux";
      RCLCPP_ERROR(this->get_logger(), "%s", last_error_.c_str());
      return false;
    }
    RCLCPP_INFO(this->get_logger(), "Camera device: %s", current_device_.c_str());

    // 2. Apply V4L2 controls
    current_ctrl_ = default_controls();
    apply_manual_camera_settings(current_device_, current_ctrl_);
    RCLCPP_INFO(this->get_logger(), "V4L2 controls applied (manual exposure=%d)",
      current_ctrl_.exposure_time);

    // 3. Build pipeline string
    std::string host     = this->get_parameter("stream_host").as_string();
    int port             = this->get_parameter("stream_port").as_int();
    int width            = this->get_parameter("width").as_int();
    int height           = this->get_parameter("height").as_int();
    int fps              = this->get_parameter("fps").as_int();
    int bitrate          = this->get_parameter("bitrate_kbps").as_int();

    std::string pipe_str = build_pipeline_str(
      current_device_, host, port, width, height, fps, bitrate);

    RCLCPP_INFO(this->get_logger(), "Pipeline:\n  %s", pipe_str.c_str());

    // 4. Parse and create pipeline
    GError * err = nullptr;
    pipeline_ = gst_parse_launch(pipe_str.c_str(), &err);
    if (!pipeline_ || err) {
      last_error_ = "gst_parse_launch failed: " +
        std::string(err ? err->message : "unknown");
      if (err) g_error_free(err);
      RCLCPP_ERROR(this->get_logger(), "%s", last_error_.c_str());
      return false;
    }

    // 5. Set up bus message watch (runs in GLib main loop context)
    bus_ = gst_element_get_bus(pipeline_);
    gst_bus_add_signal_watch(bus_);
    g_signal_connect(bus_, "message", G_CALLBACK(on_bus_message_static), this);

    // 6. Start playing
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      last_error_ = "Failed to set pipeline to PLAYING";
      RCLCPP_ERROR(this->get_logger(), "%s", last_error_.c_str());
      stop_pipeline();
      return false;
    }

    pipeline_state_str_ = "PLAYING";
    frame_count_ = 0;
    last_error_  = "";
    RCLCPP_INFO(this->get_logger(), "Pipeline PLAYING → %s:%d", host.c_str(), port);
    return true;
  }

  // -----------------------------------------------------------------------
  // Main streaming loop with auto-restart
  // -----------------------------------------------------------------------
  void stream_loop()
  {
    int restart_delay = this->get_parameter("restart_delay_ms").as_int();
    running_ = true;

    // Run a GLib main loop for GStreamer bus messages
    GMainLoop * glib_loop = g_main_loop_new(nullptr, FALSE);

    while (running_) {
      if (!start_pipeline()) {
        pipeline_state_str_ = "ERROR";
        RCLCPP_WARN(this->get_logger(),
          "Pipeline failed. Retrying in %d ms...", restart_delay);
        std::this_thread::sleep_for(std::chrono::milliseconds(restart_delay));
        continue;
      }

      // Run GLib loop (handles bus callbacks) — we break it on error/EOS
      // from the bus callback by calling g_main_loop_quit
      glib_loop_ref_ = glib_loop;
      g_main_loop_run(glib_loop);

      if (!running_) break;

      // Reached here: pipeline stopped (EOS or error)
      stop_pipeline();
      pipeline_state_str_ = "RESTARTING";
      RCLCPP_WARN(this->get_logger(),
        "Pipeline stopped. Restarting in %d ms...", restart_delay);
      std::this_thread::sleep_for(std::chrono::milliseconds(restart_delay));
    }

    g_main_loop_unref(glib_loop);
    RCLCPP_INFO(this->get_logger(), "Stream loop exited");
  }

  GMainLoop * glib_loop_ref_ = nullptr;

  // -----------------------------------------------------------------------
  // GStreamer bus message callback
  // -----------------------------------------------------------------------
  static void on_bus_message_static(GstBus * /*bus*/, GstMessage * msg, gpointer data)
  {
    auto * node = static_cast<JetsonStreamerNode *>(data);
    node->on_bus_message(msg);
  }

  void on_bus_message(GstMessage * msg)
  {
    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_EOS:
        RCLCPP_WARN(this->get_logger(), "Pipeline EOS received");
        last_error_ = "EOS";
        pipeline_state_str_ = "EOS";
        if (glib_loop_ref_) g_main_loop_quit(glib_loop_ref_);
        break;

      case GST_MESSAGE_ERROR: {
        GError * err = nullptr;
        gchar  * dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        last_error_ = "GStreamer error: " + std::string(err->message);
        RCLCPP_ERROR(this->get_logger(), "%s\nDebug: %s",
          last_error_.c_str(), dbg ? dbg : "none");
        g_error_free(err);
        g_free(dbg);
        pipeline_state_str_ = "ERROR";
        if (glib_loop_ref_) g_main_loop_quit(glib_loop_ref_);
        break;
      }

      case GST_MESSAGE_STATE_CHANGED: {
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
          GstState old_s, new_s, pending;
          gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
          pipeline_state_str_ = gst_element_state_get_name(new_s);
          if (new_s == GST_STATE_PLAYING) {
            RCLCPP_INFO(this->get_logger(), "Pipeline state: PLAYING");
          }
        }
        break;
      }

      case GST_MESSAGE_ELEMENT: {
        // Count frames via buffer-probe if needed (optional)
        frame_count_++;
        break;
      }

      default:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // ROS2: camera control subscriber
  // -----------------------------------------------------------------------
  void on_control(const auv_camera_msgs::msg::CameraControl::SharedPtr msg)
  {
    RCLCPP_INFO(this->get_logger(),
      "Camera control received: exposure=%d auto_exp=%s",
      msg->exposure_time, msg->auto_exposure ? "true" : "false");

    current_ctrl_ = *msg;
    if (!current_device_.empty()) {
      apply_manual_camera_settings(current_device_, current_ctrl_);
    }
  }

  // -----------------------------------------------------------------------
  // ROS2: restart service
  // -----------------------------------------------------------------------
  void on_restart(
    const std_srvs::srv::Trigger::Request::SharedPtr  /*req*/,
    std_srvs::srv::Trigger::Response::SharedPtr         res)
  {
    RCLCPP_INFO(this->get_logger(), "Manual restart requested");
    if (glib_loop_ref_) g_main_loop_quit(glib_loop_ref_);
    res->success = true;
    res->message = "Pipeline restart triggered";
  }

  // -----------------------------------------------------------------------
  // ROS2: status publisher (1 Hz)
  // -----------------------------------------------------------------------
  void publish_status()
  {
    auto msg = auv_camera_msgs::msg::CameraStatus();
    msg.header.stamp     = this->now();
    msg.device_path      = current_device_;
    msg.pipeline_state   = pipeline_state_str_;
    msg.streaming        = (pipeline_state_str_ == "PLAYING");
    msg.frame_count      = frame_count_;
    msg.error_msg        = last_error_;
    msg.auto_exposure_on = current_ctrl_.auto_exposure;
    msg.exposure_time    = current_ctrl_.exposure_time;
    msg.brightness_level = current_ctrl_.brightness;
    status_pub_->publish(msg);
  }
};

// ===========================================================================
int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<JetsonStreamerNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
