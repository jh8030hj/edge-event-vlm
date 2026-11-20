"""Run a TensorRT engine on images and compare with the torch model.

Usage: python src/infer_engine.py [engine] [image ...]   (defaults: models/resnet50_fp16.engine, test_data/*.jpg)
"""
import glob
import json
import sys

import tensorrt as trt
import torch
from load_resnet import load
from PIL import Image
from torchvision import transforms as T

# preprocessor_config.json: resize 256 (bicubic) -> center crop 224 -> ImageNet mean/std
prep = T.Compose([
    T.Resize(256, interpolation=T.InterpolationMode.BICUBIC), T.CenterCrop(224), T.ToTensor(),
    T.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
])


def load_engine(path):
    runtime = trt.Runtime(trt.Logger(trt.Logger.WARNING))
    with open(path, "rb") as f:
        return runtime, runtime.deserialize_cuda_engine(f.read())  # runtime must outlive the engine


def run(ctx, x):
    xc = x.cuda().contiguous()
    y = torch.empty(1, 1000, device="cuda")
    ctx.set_tensor_address("input", xc.data_ptr())
    ctx.set_tensor_address("logits", y.data_ptr())
    if not ctx.execute_async_v3(torch.cuda.current_stream().cuda_stream):  # not an assert: python -O would skip the call
        raise RuntimeError("TensorRT execution failed")
    torch.cuda.synchronize()
    return y.cpu()


if __name__ == "__main__":
    engine = sys.argv[1] if len(sys.argv) > 1 else "models/resnet50_fp16.engine"
    images = sys.argv[2:] or sorted(glob.glob("test_data/*.jpg"))
    assert images, "no images"
    id2label = json.load(open("models/config.json"))["id2label"]
    runtime, eng = load_engine(engine)
    ctx = eng.create_execution_context()
    ref_model = load("models")
    for p in images:
        x = prep(Image.open(p).convert("RGB")).unsqueeze(0)
        with torch.no_grad():
            ref = ref_model(x).logits
        out = run(ctx, x)
        print(f"\n{p}")
        for name, y in (("trt  ", out), ("torch", ref)):
            prob, idx = y.softmax(1)[0].topk(3)
            print(f"  {name}", ", ".join(f"{id2label[str(i.item())]} {q.item():.3f}" for q, i in zip(prob, idx)))
        print(f"  max abs logit diff {(ref - out).abs().max().item():.2e}, "
              f"top1 {'same' if ref.argmax() == out.argmax() else 'DIFFERENT'}")
