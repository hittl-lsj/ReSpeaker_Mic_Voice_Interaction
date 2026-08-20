cmake_minimum_required(VERSION 3.8)
project(respeaker_driver)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(voice_interfaces REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA   REQUIRED alsa)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

add_executable(respeaker_node
  src/respeaker_node.cpp
  src/audio_capture.cpp
  src/respeaker_usb.cpp
)

target_include_directories(respeaker_node PRIVATE
  ${ALSA_INCLUDE_DIRS}
  ${LIBUSB_INCLUDE_DIRS}
)

target_link_libraries(respeaker_node
  ${ALSA_LIBRARIES}
  ${LIBUSB_LIBRARIES}
)

ament_target_dependencies(respeaker_node
  rclcpp std_msgs voice_interfaces
)

install(TARGETS respeaker_node DESTINATION lib/${PROJECT_NAME})

ament_package()
