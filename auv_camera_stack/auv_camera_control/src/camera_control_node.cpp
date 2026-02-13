/**
 * auv_camera_control: camera_control_node.cpp
 *
 * Runs on: Jetson Orin Nano
 * Purpose: Bridge between ROS2 /camera/control topic and V4L2 hardware controls
 *
 * The adaptive node (on LOQ) publishes CameraControl messages here.
 * This node applies them to the physical camera immediately.
 */

#include <rclcpp/rclcpp.hpp>
#include <auv_camera_msgs/msg/camera_control.hpp>
#include <auv_camera_msgs/msg/camera_status.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <string>
#include <cstdlib>
#include <algorithm>

// Clamp helper
template<typename T>
T clamp(T val, T lo, T hi) { return std::max(lo, std::min(hi, val)); }

// ---------------------------------------------------------------------------
// Safe v4l2-ctl wrapper
// ---------------------------------------------------------------------------
static bool v4l2_set(const std::string & dev, const std::string & ctrl, int val)
{
  std::string cmd = "v4l2-ctl --device=" + dev +
    " --set-ctrl=" + ctrl + "=" + std::to_string(val) +
    " 2>&1";
  int ret = std::system(cmd.c_str());
  return (ret == 0);
}

static int v4l2_get(const std::string & dev, const std::string & ctrl)
{
  // Returns current value or -1 on failure
  char buf[256];
  std::string cmd = "v4l2-ctl --device=" + dev +
    " --get-ctrl=" + ctrl +
    " 2>/dev/null | awk -F': ' '{print $2}'";

  FILE * pipe = popen(cmd.c_str(), "r");
  if (!pipe) return -1;
  if (fgets(buf, sizeof(buf), pipe) != nullptr) {
    pclose(pipe);
    try { return std::stoi(std::string(buf)); }
    catch (...) { return -1; }
  }
  pclose(pipe);
  return -1;
}

// ===========================================================================
class CameraControlNode : public rclcpp::Node
{
public:
  CameraControlNode()
  : Node("camera_control_node")
  {
    this->declare_parameter("device", "/dev/video0");

    device_ = this->get_parameter("device").as_string();

    RCLCPP_INFO(this->get_logger(),
      "CameraControlNode started, device: %s", device_.c_str());

    // Subscribe to control commands (from LOQ adaptive node via DDS)
    ctrl_sub_ = this->create_subscription<auv_camera_msgs::msg::CameraControl>(
      "/camera/control", 10,
      std::bind(&CameraControlNode::apply_control, this, std::placeholders::_1));

    // Subscribe to status to update our device path if device is found by streamer
    status_sub_ = this->create_subscription<auv_camera_msgs::msg::CameraStatus>(
      "/camera/status", 10,
      [this](const auv_camera_msgs::msg::CameraStatus::SharedPtr msg) {
        if (!msg->device_path.empty() && msg->device_path != device_) {
          device_ = msg->device_path;
          RCLCPP_INFO(this->get_logger(),
            "Device path updated from status: %s", device_.c_str());
        }
      });

    // Service: apply safe underwater defaults immediately
    safe_defaults_srv_ = this->create_service<std_srvs::srv::Trigger>(
      "/camera/safe_defaults",
      std::bind(&CameraControlNode::on_safe_defaults, this,
        std::placeholders::_1, std::placeholders::_2));
  }

private:
  std::string device_;
  rclcpp::Subscription<auv_camera_msgs::msg::CameraControl>::SharedPtr ctrl_sub_;
  rclcpp::Subscription<auv_camera_msgs::msg::CameraStatus>::SharedPtr  status_sub_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr safe_defaults_srv_;

  // -----------------------------------------------------------------------
  // Apply CameraControl message to hardware
  // -----------------------------------------------------------------------
  void apply_control(const auv_camera_msgs::msg::CameraControl::SharedPtr msg)
  {
    RCLCPP_DEBUG(this->get_logger(),
      "Applying controls to %s", device_.c_str());

    // --- Exposure mode MUST be set before exposure value ---
    if (!msg->auto_exposure) {
      v4l2_set(device_, "auto_exposure", 1);  // Manual
      // Clamp to camera's supported range (determined by testing)
      int exp = clamp(msg->exposure_time, 1, 5000);
      v4l2_set(device_, "exposure_time_absolute", exp);
    } else {
      v4l2_set(device_, "auto_exposure", 3);  // Aperture Priority
    }

    // --- White balance ---
    if (!msg->white_balance_auto) {
      v4l2_set(device_, "white_balance_automatic", 0);
      int wb = clamp(msg->white_balance_temp, 2800, 6500);
      v4l2_set(device_, "white_balance_temperature", wb);
    } else {
      v4l2_set(device_, "white_balance_automatic", 1);
    }

    // --- Other controls with range clamping ---
    v4l2_set(device_, "brightness",
      clamp(msg->brightness, -64, 64));
    v4l2_set(device_, "contrast",
      clamp(msg->contrast, 0, 64));
    v4l2_set(device_, "saturation",
      clamp(msg->saturation, 0, 128));
    v4l2_set(device_, "hue",
      clamp(msg->hue, -40, 40));
    v4l2_set(device_, "gamma",
      clamp(msg->gamma, 72, 500));
    v4l2_set(device_, "gain",
      clamp(msg->gain, 0, 100));
    v4l2_set(device_, "sharpness",
      clamp(msg->sharpness, 0, 6));
    v4l2_set(device_, "backlight_compensation",
      clamp(msg->backlight_comp, 0, 160));
    v4l2_set(device_, "power_line_frequency",
      clamp(msg->power_line_freq, 0, 2));

    RCLCPP_INFO(this->get_logger(),
      "Controls applied: exp=%d auto_exp=%s sat=%d gain=%d",
      msg->exposure_time,
      msg->auto_exposure ? "AUTO" : "MANUAL",
      msg->saturation, msg->gain);
  }

  // -----------------------------------------------------------------------
  // Apply safe underwater defaults
  // Specifically tuned for Chennai 2PM–6PM outdoor/underwater conditions
  // -----------------------------------------------------------------------
  void on_safe_defaults(
    const std_srvs::srv::Trigger::Request::SharedPtr  /*req*/,
    std_srvs::srv::Trigger::Response::SharedPtr         res)
  {
    RCLCPP_INFO(this->get_logger(), "Applying safe underwater defaults...");

    // Step 1: Kill auto modes first
    v4l2_set(device_, "auto_exposure",          1);   // Manual mode
    v4l2_set(device_, "white_balance_automatic", 0);  // Manual WB

    // Step 2: Set safe values
    v4l2_set(device_, "exposure_time_absolute",  300); // Conservative, not saturated
    v4l2_set(device_, "white_balance_temperature", 5500); // Neutral-cool for water
    v4l2_set(device_, "brightness",              0);
    v4l2_set(device_, "contrast",               42);   // Slightly above default
    v4l2_set(device_, "saturation",             80);   // Boost for underwater color loss
    v4l2_set(device_, "hue",                     6);
    v4l2_set(device_, "gamma",                 100);
    v4l2_set(device_, "gain",                   15);   // Low gain = less noise
    v4l2_set(device_, "sharpness",               3);
    v4l2_set(device_, "backlight_compensation",  0);   // OFF: prevents whiteout
    v4l2_set(device_, "power_line_frequency",    1);   // 50 Hz (India)

    res->success = true;
    res->message = "Safe underwater defaults applied";
    RCLCPP_INFO(this->get_logger(), "%s", res->message.c_str());
  }
};

// ===========================================================================
int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<CameraControlNode>());
  rclcpp::shutdown();
  return 0;
}
