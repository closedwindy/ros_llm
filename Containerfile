    # Containerfile
    FROM docker.io/osrf/ros:humble-desktop


    ENV DEBIAN_FRONTEND=noninteractive

    RUN apt update && apt install -y \
        python3-pip \
        python3-flask \          
        python3-requests \
        vim \
        git \
        && rm -rf /var/lib/apt/lists/*


    WORKDIR /ros2_ws

    COPY ./ros_ws/src ./src/


    COPY ./llm ./llm/


    COPY ./llm/requirements.txt ./llm/
    RUN pip3 install -r ./llm/requirements.txt

    RUN rosdep update && \
        rosdep install --from-paths src --ignore-src -y --skip-keys="librealsense2" && \
        colcon build --symlink-install


    CMD ["bash"]