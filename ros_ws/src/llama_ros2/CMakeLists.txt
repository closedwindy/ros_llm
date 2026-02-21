cmake_minimum_required(VERSION 3.8)
project(llama_ros2)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 17)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)


set(LLAMA_STANDALONE OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_DOCS OFF CACHE BOOL "" FORCE)

set(GGML_CUDA ON CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)

add_subdirectory(3rdparty/llama.cpp EXCLUDE_FROM_ALL)


add_executable(llama_server_node src/llama_server_node.cpp)

target_link_libraries(llama_server_node 
    llama
    ggml
    ${CMAKE_THREAD_LIBS_INIT}
)

ament_target_dependencies(llama_server_node 
    rclcpp
)


install(TARGETS llama_server_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()