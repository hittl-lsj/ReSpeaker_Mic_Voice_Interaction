cmake_minimum_required(VERSION 3.8)
project(voice_interaction)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(voice_interfaces REQUIRED)

# 系统库
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)

# sherpa-onnx C API（开发机 x86_64 / X5 aarch64 各自下载对应版本）
set(SHERPA_ONNX_ROOT "$ENV{SHERPA_ONNX_ROOT}" CACHE PATH "sherpa-onnx install prefix")
find_library(SHERPA_C_API_LIB sherpa-onnx-c-api PATHS ${SHERPA_ONNX_ROOT}/lib NO_DEFAULT_PATH)
find_library(ONNXRUNTIME_LIB onnxruntime PATHS ${SHERPA_ONNX_ROOT}/lib NO_DEFAULT_PATH)
include_directories(${SHERPA_ONNX_ROOT}/include)
if(NOT SHERPA_C_API_LIB OR NOT ONNXRUNTIME_LIB)
  message(FATAL_ERROR "未找到 sherpa-onnx 库。请下载对应架构的 sherpa-onnx 包并 export SHERPA_ONNX_ROOT=<解压目录>（该目录需含 lib/ 与 include/）。")
endif()

add_executable(interaction_node
  src/interaction_node.cpp
  src/http_client.cpp
  src/llm_client.cpp
  src/asr_sherpa.cpp
  src/keyword_spotter.cpp
  src/tts_edge.cpp
  src/tts_piper.cpp
  src/speech_recorder.cpp
  src/playback_manager.cpp
  src/response_streamer.cpp
  src/session_state_machine.cpp
)

target_include_directories(interaction_node PRIVATE
  ${CURL_INCLUDE_DIRS}
)

target_link_libraries(interaction_node
  ${CURL_LIBRARIES}
  ${SHERPA_C_API_LIB}
  ${ONNXRUNTIME_LIB}
)

# 运行时能找到 libsherpa-onnx-c-api.so / libonnxruntime.so
set_target_properties(interaction_node PROPERTIES
  BUILD_RPATH "${SHERPA_ONNX_ROOT}/lib"
  INSTALL_RPATH "${SHERPA_ONNX_ROOT}/lib")

ament_target_dependencies(interaction_node
  rclcpp std_msgs voice_interfaces
)

install(TARGETS interaction_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY launch config
  DESTINATION share/${PROJECT_NAME}
  PATTERN "__pycache__" EXCLUDE
  PATTERN "*.pyc" EXCLUDE
)

ament_package()
