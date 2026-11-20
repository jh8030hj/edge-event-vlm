"""Latency of torch (FP32/FP16) vs TensorRT engines, batch 1, input resident on GPU.

Usage: python src/bench_compare.py [warmup] [iters]
Each case is timed the same way: GPU-synchronized wall time around one forward pass.
"""
import gc
import statistics
import sys
import time

import torch
from infer_engine import load_engine
from load_resnet import load

WARMUP = int(sys.argv[1]) if len(sys.argv) > 1 else 50
ITERS = int(sys.argv[2]) if len(sys.argv) > 2 else 200


def bench(fn):
    for _ in range(WARMUP):
        fn()
    ts = []
    for _ in range(ITERS):
        torch.cuda.synchronize()  # without this we would time only the kernel launches
        t = time.perf_counter()
        fn()
        torch.cuda.synchronize()
        ts.append((time.perf_counter() - t) * 1e3)
    ts.sort()
    return statistics.mean(ts), statistics.median(ts), ts[int(len(ts) * 0.99) - 1]


def torch_case(dtype):
    m = load("models").cuda().to(dtype)
    x = torch.randn(1, 3, 224, 224, device="cuda", dtype=dtype)
    with torch.inference_mode():
        return bench(lambda: m(x))


def trt_case(path):
    runtime, eng = load_engine(path)
    ctx = eng.create_execution_context()
    x = torch.randn(1, 3, 224, 224, device="cuda")
    y = torch.empty(1, 1000, device="cuda")
    ctx.set_tensor_address("input", x.data_ptr())
    ctx.set_tensor_address("logits", y.data_ptr())
    stream = torch.cuda.current_stream().cuda_stream

    def fn():
        if not ctx.execute_async_v3(stream):
            raise RuntimeError("TensorRT execution failed")

    return bench(fn)


if __name__ == "__main__":
    cases = [
        ("torch FP32", lambda: torch_case(torch.float32)),
        ("torch FP16", lambda: torch_case(torch.float16)),
        ("TensorRT FP32", lambda: trt_case("models/resnet50_fp32.engine")),
        ("TensorRT FP16", lambda: trt_case("models/resnet50_fp16.engine")),
    ]
    print(f"warmup {WARMUP}, iters {ITERS}, batch 1\n{'case':<15}{'mean ms':>9}{'median':>9}{'p99':>9}{'qps(mean)':>11}")
    for name, f in cases:
        mean, med, p99 = f()
        print(f"{name:<15}{mean:>9.2f}{med:>9.2f}{p99:>9.2f}{1000 / mean:>11.0f}")
        gc.collect()
        torch.cuda.empty_cache()  # one model at a time: memory is shared and tight
