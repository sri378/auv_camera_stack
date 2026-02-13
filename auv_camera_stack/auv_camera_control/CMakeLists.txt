cmake_minimum_required(VERSION 3.8)
project(auv_camera_control)

find_package(ament_cmake    REQUIRED)
find_package(rclcpp          REQUIRED)
find_package(std_srvs        REQUIRED)
find_package(auv_camera_msgs REQUIRED)

add_executable(camera_control_node
  src/camera_control_node.cpp
)

ament_target_dependencies(camera_control_node
  rclcpp
  std_srvs
  auv_camera_msgs
)

install(TARGETS camera_control_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
