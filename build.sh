#!/bin/bash
set -e

echo "[BUILD] Compiling sherpa_kws_demo for ARM..."

${CXX} \
    -o sherpa_kws_demo \
    sherpa_kws_demo.cpp \
    -I${SYSROOT}/usr/include \
    -I/build/install/include \
    -I. \
    -L/build/install/lib \
    -L${SYSROOT}/usr/lib/arm-linux-gnueabihf \
    -lsherpa-onnx-c-api \
    -lonnxruntime \
    -lasound \
    -lpthread \
    -std=c++14 \
    -static-libstdc++ \
    -static-libgcc \
    -O2 \
    -Wl,-rpath,/data/audio/lib

echo "[BUILD] Build successful!"
echo "[BUILD] Output: sherpa_kws_demo"