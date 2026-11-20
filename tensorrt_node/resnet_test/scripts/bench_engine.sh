#!/usr/bin/env bash
# Usage: ./bench_engine.sh [engine]   (latency/throughput only; random input, use infer_engine.py for correctness)
set -euo pipefail
cd "$(dirname "$0")/.."

/usr/src/tensorrt/bin/trtexec --loadEngine="${1:-models/resnet50_fp16.engine}"
