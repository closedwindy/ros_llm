FROM docker.io/osrf/ros:humble-desktop

ENV DEBIAN_FRONTEND=noninteractive
ENV http_proxy="" https_proxy=""

# 替换为 TUNA 源（加速）
RUN echo "deb https://mirrors.tuna.tsinghua.edu.cn/ubuntu jammy main restricted universe multiverse" > /etc/apt/sources.list && \
    echo "deb https://mirrors.tuna.tsinghua.edu.cn/ubuntu jammy-updates main restricted universe multiverse" >> /etc/apt/sources.list && \
    echo "deb https://mirrors.tuna.tsinghua.edu.cn/ubuntu jammy-backports main restricted universe multiverse" >> /etc/apt/sources.list && \
    echo "deb https://mirrors.tuna.tsinghua.edu.cn/ubuntu jammy-security main restricted universe multiverse" >> /etc/apt/sources.list && \
    apt update && \
    apt install -y curl gnupg && \
    curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key -o /usr/share/keyrings/ros-archive-keyring.gpg && \
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] https://mirrors.tuna.tsinghua.edu.cn/ros2/ubuntu jammy main" > /etc/apt/sources.list.d/ros2.list

# 安装完整 ROS 2 C++ 开发依赖
RUN apt update && apt install -y \
    python3-pip \
    python3-flask \
    python3-requests \
    vim \
    git \
    # --- 关键：ament_cmake 完整套件 ---
    ros-humble-ament-cmake \
    ros-humble-ament-cmake-core \
    ros-humble-ament-cmake-libraries \
    ros-humble-ament-cmake-python \
    ros-humble-ament-lint-auto \
    ros-humble-ament-lint-common \
    ros-humble-rosidl-default-generators \
    ros-humble-rclcpp \
    build-essential \
    cmake \
    libpython3-dev \
    python3-colcon-common-extensions \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /ros2_ws
COPY ./ros_ws/src ./src/
COPY ./llm ./llm/
COPY ./llm/requirements.txt ./llm/

RUN pip3 install -r ./llm/requirements.txt

# 构建（禁用 CUDA，ARM64 不支持）
RUN unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY && \
    rosdep install --from-paths src --ignore-src -y --skip-keys="librealsense2" && \
    colcon build --packages-select llama_ros2 \
        --cmake-args -DGGML_CUDA=OFF \
        --parallel-workers $(nproc)

CMD ["bash"]