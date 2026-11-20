#!/usr/bin/env bash
# Usage: ./build_engine.sh [fp16|fp32] [onnx] [engine]
set -euo pipefail
cd "$(dirname "$0")"   # models/ paths are relative to resnet_test/

prec=${1:-fp16}
onnx=${2:-models/resnet50.onnx}
engine=${3:-models/resnet50_${prec}.engine}
flag=""; [ "$prec" = fp16 ] && flag="--fp16"

/usr/src/tensorrt/bin/trtexec --onnx="$onnx" --saveEngine="$engine" $flag
