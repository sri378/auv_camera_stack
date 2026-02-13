/**
 * Receives: H264/RTP UDP on port 5600 from Jetson 
 * Publishes: /camera/image_raw  (sensor_msgs/Image, BGR8)
 *            /camera/image_compressed (compressed JPEG)
 *
 * GStreamer pipeline:
 *   udpsrc → rtph264depay → h264parse → avdec_h264 → videoconvert → appsink
 
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <std_msgs/msg/float32.hpp>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <glib.h>

#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <sstream>

// ===========================================================================
class CameraReceiverNode : public rclcpp::Node
{
public:
  CameraReceiverNode()
  : Node("camera_receiver"),
    pipeline_(nullptr),
    appsink_(nullptr),
    running_(false),
    frame_count_(0)
  {
    this->declare_parameter("listen_port",    5600);
    this->declare_parameter("buffer_size",    524288);  // 512 KB UDP buffer
    this->declare_parameter("frame_id",       "camera");
    this->declare_parameter("publish_compressed", true);

    image_pub_ = this->create_publisher<sensor_msgs::msg::Image>(
      "/camera/image_raw", 5);

    compressed_pub_ = this->create_publisher<sensor_msgs::msg::CompressedImage>(
      "/camera/image_compressed", 5);

    brightness_pub_ = this->create_publisher<std_msgs::msg::Float32>(
      "/camera/brightness", 10);

    gst_init(nullptr, nullptr);

    stream_thread_ = std::thread([this]() { receive_loop(); });

    RCLCPP_INFO(this->get_logger(), "CameraReceiver node started");
  }

  ~CameraReceiverNode()
  {
    running_ = false;
    stop_pipeline();
    if (stream_thread_.joinable()) stream_thread_.join();
  }

private:
  GstElement * pipeline_;
  GstElement * appsink_;
  GMainLoop  * glib_loop_ = nullptr;
  std::atomic<bool> running_;
  std::atomic<uint32_t> frame_count_;
  std::thread stream_thread_;

  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr          image_pub_;
  rclcpp::Publisher<sensor_msgs::msg::CompressedImage>::SharedPtr compressed_pub_;
  rclcpp::Publisher<std_msgs::msg::Float32>::SharedPtr            brightness_pub_;

  // -----------------------------------------------------------------------
  // Build receive pipeline
  //   udpsrc → depay → parse → decode → convert → appsink
  // -----------------------------------------------------------------------
  std::string build_rx_pipeline(int port, int buf_size)
  {
    std::ostringstream ss;
    ss << "udpsrc port=" << port
       << " buffer-size=" << buf_size
       << " ! application/x-rtp,media=video,clock-rate=90000,"
          "encoding-name=H264,payload=96"
       << " ! rtph264depay"
       << " ! h264parse"
       << " ! avdec_h264 max-threads=2"
       << " ! videoconvert"
       << " ! video/x-raw,format=BGR"
       << " ! appsink name=sink"
          " max-buffers=2"          // Keep only latest 2 frames
          " drop=true"              // Drop old frames under load
          " sync=false"
          " emit-signals=true";
    return ss.str();
  }

  // -----------------------------------------------------------------------
  void stop_pipeline()
  {
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      if (glib_loop_) g_main_loop_quit(glib_loop_);
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
      appsink_  = nullptr;
    }
  }

  // -----------------------------------------------------------------------
  bool start_pipeline()
  {
    stop_pipeline();

    int port     = this->get_parameter("listen_port").as_int();
    int buf_size = this->get_parameter("buffer_size").as_int();

    std::string pipe_str = build_rx_pipeline(port, buf_size);
    RCLCPP_INFO(this->get_logger(), "RX Pipeline:\n  %s", pipe_str.c_str());

    GError * err = nullptr;
    pipeline_ = gst_parse_launch(pipe_str.c_str(), &err);
    if (!pipeline_ || err) {
      RCLCPP_ERROR(this->get_logger(), "Pipeline parse failed: %s",
        err ? err->message : "unknown");
      if (err) g_error_free(err);
      return false;
    }

    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if (!appsink_) {
      RCLCPP_ERROR(this->get_logger(), "appsink element not found");
      stop_pipeline();
      return false;
    }

    // Connect new-sample signal
    g_signal_connect(appsink_, "new-sample",
      G_CALLBACK(on_new_sample_static), this);

    // Bus for errors
    GstBus * bus = gst_element_get_bus(pipeline_);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message", G_CALLBACK(on_bus_msg_static), this);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      RCLCPP_ERROR(this->get_logger(), "Failed to start pipeline");
      stop_pipeline();
      return false;
    }

    RCLCPP_INFO(this->get_logger(), "Receiver pipeline PLAYING on port %d", port);
    return true;
  }

  void receive_loop()
  {
    running_ = true;
    glib_loop_ = g_main_loop_new(nullptr, FALSE);

    while (running_) {
      if (!start_pipeline()) {
        RCLCPP_WARN(this->get_logger(), "Receiver start failed, retry in 3s...");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        continue;
      }
      g_main_loop_run(glib_loop_);
      if (!running_) break;

      RCLCPP_WARN(this->get_logger(), "Pipeline stopped, restarting in 3s...");
      stop_pipeline();
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    g_main_loop_unref(glib_loop_);
  }

  // -----------------------------------------------------------------------
  // New sample callback: convert GstBuffer → ROS2 Image
  // -----------------------------------------------------------------------
  static GstFlowReturn on_new_sample_static(GstAppSink * sink, gpointer data)
  {
    return static_cast<CameraReceiverNode *>(data)->on_new_sample(sink);
  }

  GstFlowReturn on_new_sample(GstAppSink * sink)
  {
    GstSample * sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer * buffer = gst_sample_get_buffer(sample);
    GstCaps   * caps   = gst_sample_get_caps(sample);

    // Get image dimensions from caps
    GstStructure * s = gst_caps_get_structure(caps, 0);
    int width = 0, height = 0;
    gst_structure_get_int(s, "width",  &width);
    gst_structure_get_int(s, "height", &height);

    // Map buffer
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      gst_sample_unref(sample);
      return GST_FLOW_OK;
    }

    // Build sensor_msgs/Image (BGR8)
    auto img_msg = sensor_msgs::msg::Image();
    img_msg.header.stamp    = this->now();
    img_msg.header.frame_id = this->get_parameter("frame_id").as_string();
    img_msg.width           = static_cast<uint32_t>(width);
    img_msg.height          = static_cast<uint32_t>(height);
    img_msg.encoding        = "bgr8";
    img_msg.step            = static_cast<uint32_t>(width * 3);
    img_msg.data.assign(map.data, map.data + map.size);

    image_pub_->publish(img_msg);

    // Calculate average brightness for adaptive control (sample every 5th frame)
    frame_count_++;
    if (frame_count_ % 5 == 0) {
      publish_brightness(map.data, map.size);
    }

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  void publish_brightness(const guint8 * data, gsize size)
  {
    if (size == 0) return;
    uint64_t sum = 0;
    gsize step  = 300;  // Sample every 100th pixel (3 bytes per pixel BGR)
    gsize count = 0;
    for (gsize i = 0; i < size - 2; i += step) {
      // Luminance ≈ 0.114*B + 0.587*G + 0.299*R
      sum += static_cast<uint64_t>(
        0.114 * data[i] + 0.587 * data[i+1] + 0.299 * data[i+2]);
      count++;
    }

    if (count > 0) {
      auto msg = std_msgs::msg::Float32();
      msg.data = static_cast<float>(sum) / static_cast<float>(count);
      brightness_pub_->publish(msg);
    }
  }

  // -----------------------------------------------------------------------
  static void on_bus_msg_static(GstBus * /*bus*/, GstMessage * msg, gpointer data)
  {
    auto * node = static_cast<CameraReceiverNode *>(data);
    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_ERROR: {
        GError * err = nullptr; gchar * dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        RCLCPP_ERROR(node->get_logger(), "RX Pipeline error: %s | %s",
          err->message, dbg ? dbg : "");
        g_error_free(err); g_free(dbg);
        if (node->glib_loop_) g_main_loop_quit(node->glib_loop_);
        break;
      }
      case GST_MESSAGE_EOS:
        RCLCPP_WARN(node->get_logger(), "RX Pipeline EOS");
        if (node->glib_loop_) g_main_loop_quit(node->glib_loop_);
        break;
      default: break;
    }
  }
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<CameraReceiverNode>());
  rclcpp::shutdown();
  return 0;
}
