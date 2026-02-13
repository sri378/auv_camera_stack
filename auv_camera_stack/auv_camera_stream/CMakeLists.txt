cmake_minimum_required(VERSION 3.8)
project(auv_camera_stream)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -O2)
endif()

find_package(ament_cmake      REQUIRED)
find_package(rclcpp            REQUIRED)
find_package(std_msgs          REQUIRED)
find_package(std_srvs          REQUIRED)
find_package(auv_camera_msgs   REQUIRED)

# GStreamer
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
  gstreamer-1.0
  gstreamer-base-1.0
  glib-2.0
)

include_directories(${GST_INCLUDE_DIRS})

add_executable(jetson_streamer_node
  src/jetson_streamer_node.cpp
)

ament_target_dependencies(jetson_streamer_node
  rclcpp
  std_msgs
  std_srvs
  auv_camera_msgs
)

target_link_libraries(jetson_streamer_node
  ${GST_LIBRARIES}
)

install(TARGETS jetson_streamer_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
