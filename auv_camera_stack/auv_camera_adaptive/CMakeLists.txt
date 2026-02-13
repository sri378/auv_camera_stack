cmake_minimum_required(VERSION 3.8)
project(auv_camera_adaptive)

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_python REQUIRED)
find_package(rclpy REQUIRED)
find_package(std_msgs REQUIRED)
find_package(auv_camera_msgs REQUIRED)

ament_python_install_package(${PROJECT_NAME})

install(PROGRAMS
  src/adaptive_controller_node.py
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
