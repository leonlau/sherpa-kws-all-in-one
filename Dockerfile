FROM debian:10 AS base
RUN echo "deb [arch=amd64] http://archive.debian.org/debian buster main" > /etc/apt/sources.list && \
    echo "deb http://archive.debian.org/debian-security buster/updates main" >> /etc/apt/sources.list && \
    echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99no-check-valid-until

# ============ Stage 1: Build crosstool-ng ============
FROM base AS crosstool-builder
RUN apt-get update && apt install -y build-essential cmake git wget autoconf automake libtool texinfo help2man libtool-bin libncurses5-dev flex unzip gawk  bison rsync && rm -rf /var/lib/apt/lists/*
RUN groupadd -r ubuntu && useradd -m -g ubuntu -u 1000 ubuntu
RUN  cd /opt && git clone https://github.com/crosstool-ng/crosstool-ng && chown  1000:1000 -R crosstool-ng

USER ubuntu
RUN cd /opt/crosstool-ng && ./bootstrap && ./configure --enable-local && make
COPY gcc9.config  /opt/crosstool-ng/.config
RUN cd /opt/crosstool-ng  && CT_JOBS=$(nproc) ./ct-ng  build

# ============ Stage 2: Build dependencies ============
FROM base AS deps-builder
RUN apt-get update && apt-get install -y  build-essential cmake git wget libtool m4 automake  alsa-utils pkg-config  && rm -rf /var/lib/apt/lists/*

# Install newer CMake (Sherpa-ONNX requires CMake 3.15+)
RUN cd /opt && wget https://github.com/Kitware/CMake/releases/download/v3.28.6/cmake-3.28.6-linux-x86_64.tar.gz && tar -xzf cmake-3.28.6-linux-x86_64.tar.gz -C /opt && \
    rm cmake-3.28.6-linux-x86_64.tar.gz && \
    ln -sf /opt/cmake-3.28.6-linux-x86_64/bin/cmake /usr/local/bin/cmake && \
    ln -sf /opt/cmake-3.28.6-linux-x86_64/bin/ctest /usr/local/bin/ctest

# 安装 python3.10
RUN apt-get update && apt install -y build-essential wget curl libssl-dev zlib1g-dev libbz2-dev libreadline-dev libsqlite3-dev libffi-dev libncurses5-dev libncursesw5-dev libgdbm-dev tk-dev xz-utils && rm -rf /var/lib/apt/lists/*
RUN cd /opt && wget https://www.python.org/ftp/python/3.10.20/Python-3.10.20.tar.xz && tar xf Python-3.10.20.tar.xz && cd Python-3.10.20 &&  ./configure --prefix=/usr/local/python3.10 --enable-optimizations --with-ensurepip=install && make -j$(nproc) && make install && rm /opt/Python-3.10.20.tar.xz

# ============ Stage 3: Build onnxruntime ============
FROM base AS onnxruntime-builder
RUN apt-get update && apt-get install -y  build-essential cmake git wget libtool m4 automake  alsa-utils pkg-config python3 && rm -rf /var/lib/apt/lists/*
RUN cd /opt && wget https://github.com/Kitware/CMake/releases/download/v3.28.6/cmake-3.28.6-linux-x86_64.tar.gz && tar -xzf cmake-3.28.6-linux-x86_64.tar.gz -C /opt && \
    rm cmake-3.28.6-linux-x86_64.tar.gz && \
    ln -sf /opt/cmake-3.28.6-linux-x86_64/bin/cmake /usr/local/bin/cmake && \
    ln -sf /opt/cmake-3.28.6-linux-x86_64/bin/ctest /usr/local/bin/ctest

RUN cd /opt && git clone --depth 1 --branch v1.21.1 https://github.com/microsoft/onnxruntime && cd onnxruntime && sed -i 's/5ea4d05e62d7f954a46b3213f9b2535bdd866803/51982be81bbe52572b54180454df11a3ece9a934/g' ./cmake/deps.txt &&  python3 ./tools/ci_build/build.py \
            --compile_no_warning_as_error \
            --build_dir ./build-arm \
            --config Release \
            --build \
            --build_shared_lib \
            --update \
            --cmake_extra_defines onnxruntime_BUILD_UNIT_TESTS=OFF \
            --cmake_extra_defines CMAKE_INSTALL_PREFIX=./build-arm/install/ \
            --cmake_extra_defines CMAKE_SYSTEM_NAME=Linux \
            --cmake_extra_defines CMAKE_SYSTEM_PROCESSOR=armv7l \
            --cmake_extra_defines CMAKE_C_FLAGS="-mfpu=neon -mfloat-abi=hard -march=armv7-a" \
            --cmake_extra_defines CMAKE_CXX_FLAGS="-mfpu=neon -mfloat-abi=hard -march=armv7-a -Wno-error -fpermissive -static-libstdc++ -static-libgcc" \
            --target install \
            --parallel \
            --skip_tests \
            --allow_running_as_root

# ============ Stage 4: Build sherpa-onnx ============
FROM base AS sherpa-onnx-builder
RUN groupadd -r ubuntu && useradd -m -g ubuntu -u 1000 ubuntu
RUN apt-get update && apt-get install -y  build-essential cmake git wget libtool m4 automake  alsa-utils pkg-config python3 && rm -rf /var/lib/apt/lists/*
RUN cd /opt && wget https://github.com/Kitware/CMake/releases/download/v3.28.6/cmake-3.28.6-linux-x86_64.tar.gz && tar -xzf cmake-3.28.6-linux-x86_64.tar.gz -C /opt && \
    rm cmake-3.28.6-linux-x86_64.tar.gz && \
    ln -sf /opt/cmake-3.28.6-linux-x86_64/bin/cmake /usr/local/bin/cmake && \
    ln -sf /opt/cmake-3.28.6-linux-x86_64/bin/ctest /usr/local/bin/ctest

# Copy crosstool-ng toolchain from stage 1
COPY --from=crosstool-builder /home/ubuntu/x-tools /home/ubuntu/x-tools

# Copy onnxruntime from stage 3
COPY --from=onnxruntime-builder /opt/onnxruntime/build-arm /opt/onnxruntime/build-arm

ENV PATH="/home/ubuntu/x-tools/arm-unknown-linux-gnueabihf/bin:${PATH}"
ENV SYSROOT=/home/ubuntu/x-tools/arm-unknown-linux-gnueabihf/arm-unknown-linux-gnueabihf/sysroot

# Download alsa dev packages for ARM
RUN cd /tmp && wget 'http://archive.debian.org/debian/pool/main/a/alsa-lib/libasound2-dev_1.1.3-5_armhf.deb' && wget 'http://archive.debian.org/debian/pool/main/a/alsa-lib/libasound2_1.1.3-5_armhf.deb' && dpkg-deb -x libasound2_1.1.3-5_armhf.deb $SYSROOT && dpkg-deb -x libasound2-dev_1.1.3-5_armhf.deb $SYSROOT

# Download and build sherpa-onnx for ARM
RUN cd /opt && git clone --depth 1 https://github.com/k2-fsa/sherpa-onnx.git && \
    cd sherpa-onnx && \
    mkdir build-arm && \
    cd build-arm && \
    /usr/local/bin/cmake \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_SYSTEM_PROCESSOR=armv7l \
      -DCMAKE_SYSROOT=${SYSROOT} \
      -DCMAKE_LIBRARY_ARCHITECTURE=arm-linux-gnueabihf \
      -DCMAKE_EXE_LINKER_FLAGS="-L${SYSROOT}/usr/lib/arm-linux-gnueabihf" \
      -DCMAKE_SHARED_LINKER_FLAGS="-L${SYSROOT}/usr/lib/arm-linux-gnueabihf" \
      -DCMAKE_FIND_ROOT_PATH=${SYSROOT} \
      -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
      -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
      -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
      -DCMAKE_CXX_FLAGS="-static-libstdc++ -static-libgcc" \
      -DBUILD_SHARED_LIBS=ON \
      -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
      -DSHERPA_ONNX_ENABLE_TESTS=OFF \
      -DSHERPA_ONNX_ENABLE_CHECK=OFF \
      -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
      -DSHERPA_ONNX_ENABLE_JNI=OFF \
      -DSHERPA_ONNX_ENABLE_C_API=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/build/install \
      .. && \
    make -j$(nproc) && \
    make install

# ============ Final Stage: Collect all artifacts ============
FROM debian:10
RUN echo "deb [arch=amd64] http://archive.debian.org/debian buster main" > /etc/apt/sources.list && \
    echo "deb http://archive.debian.org/debian-security buster/updates main" >> /etc/apt/sources.list && \
    echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99no-check-valid-until

# Install runtime dependencies
RUN apt-get update && apt-get install -y wget && rm -rf /var/lib/apt/lists/*

# Copy all build artifacts into a single image
COPY --from=crosstool-builder /home/ubuntu/x-tools /home/ubuntu/x-tools
COPY --from=onnxruntime-builder /opt/onnxruntime /opt/onnxruntime
COPY --from=sherpa-onnx-builder /opt/sherpa-onnx /opt/sherpa-onnx

# Set up environment
ENV PATH="/home/ubuntu/x-tools/arm-unknown-linux-gnueabihf/bin:${PATH}"
ENV SYSROOT=/home/ubuntu/x-tools/arm-unknown-linux-gnueabihf/arm-unknown-linux-gnueabihf/sysroot
ENV SHERPA_ONNXRUNTIME_LIB_DIR=/opt/onnxruntime/build-arm/Release/build-arm/install/lib
ENV SHERPA_ONNXRUNTIME_INCLUDE_DIR=/opt/onnxruntime/build-arm/Release/build-arm/install/include/onnxruntime

# Working directory
WORKDIR /build

# Copy source files and scripts
COPY sherpa_kws_demo.cpp /build/
COPY build.sh /build/
COPY pack-artifacts.sh /pack-artifacts.sh

# Build the demo
RUN /build/build.sh

CMD ["/pack-artifacts.sh"]

# Build:
# docker build -t sherpa-kws-all-in-one .

# Run:
# docker run --rm -v "$(pwd)/output:/host-output" sherpa-kws-all-in-one
