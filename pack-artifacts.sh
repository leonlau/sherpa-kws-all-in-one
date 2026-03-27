#!/bin/bash
set -e

OUTPUT_DIR=${1:-/host-output}

echo "Packing artifacts to $OUTPUT_DIR..."

# Pack crosstool-ng toolchain
if [ -d "/home/ubuntu/x-tools" ]; then
    echo "Packing x-tools..."
    tar -czf ${OUTPUT_DIR}/x-tools.tar.gz -C /home/ubuntu x-tools
fi

# Pack onnxruntime
if [ -d "/opt/onnxruntime" ]; then
    echo "Packing onnxruntime..."
    tar -czf ${OUTPUT_DIR}/onnxruntime.tar.gz -C /opt onnxruntime
fi

# Pack sherpa-onnx
if [ -d "/opt/sherpa-onnx" ]; then
    echo "Packing sherpa-onnx..."
    tar -czf ${OUTPUT_DIR}/sherpa-onnx.tar.gz -C /opt sherpa-onnx
fi

echo "Done packing artifacts."
ls -lh ${OUTPUT_DIR}/
