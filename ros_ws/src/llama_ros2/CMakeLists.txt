cmake_minimum_required(VERSION 3.8)
project(llama_ros2)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
#llama.cpp 要求
set(CMAKE_CXX_STANDARD 17)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(std_msgs REQUIRED) 
find_package(rosidl_default_generators REQUIRED)

# llama.cpp 集成 
set(LLAMA_STANDALONE OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER ON CACHE BOOL "" FORCE)

set(LLAMA_CURL ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

set(GGML_CCACHE ON CACHE BOOL "" FORCE)

set(GGML_CUDA ON CACHE BOOL "" FORCE)
set(GGML_OPENMP ON CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)    

set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
set(GGML_RPC OFF CACHE BOOL "" FORCE)
set(GGML_BLAS OFF CACHE BOOL "" FORCE)


add_subdirectory(3rdparty/llama.cpp EXCLUDE_FROM_ALL)

# Action
rosidl_generate_interfaces(${PROJECT_NAME}
  "action/LLMInfer.action"
  DEPENDENCIES std_msgs
)

add_executable(llama_server_node src/llama_server_node.cpp)

target_link_libraries(llama_server_node 
    llama
    ggml
    ${CMAKE_THREAD_LIBS_INIT}
)

ament_target_dependencies(llama_server_node 
    rclcpp
    rclcpp_action
    std_msgs  
)

# !!! 链接 Action 接口
rosidl_target_interfaces(llama_server_node 
  ${PROJECT_NAME} 
  "rosidl_typesupport_cpp"
)

ament_export_dependencies(rosidl_default_runtime)

install(TARGETS llama_server_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()