cmake_minimum_required(VERSION 3.8)
project(auv_camera_receiver)

find_package(ament_cmake    REQUIRED)
find_package(rclcpp          REQUIRED)
find_package(sensor_msgs     REQUIRED)
find_package(std_msgs        REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
  gstreamer-1.0
  gstreamer-base-1.0
  gstreamer-app-1.0
  glib-2.0
)

include_directories(${GST_INCLUDE_DIRS})

add_executable(camera_receiver_node
  src/camera_receiver_node.cpp
)

ament_target_dependencies(camera_receiver_node
  rclcpp
  sensor_msgs
  std_msgs
)

target_link_libraries(camera_receiver_node
  ${GST_LIBRARIES}
)

install(TARGETS camera_receiver_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
