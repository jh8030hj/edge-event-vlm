"""Export the ResNet-50 checkpoint to a static 1x3x224x224 ONNX (dynamo exporter)."""
import os
import sys

import onnx
import onnxruntime as ort
import torch
from load_resnet import load


class Logits(torch.nn.Module):  # return the tensor, not the HF output object
    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, x):
        return self.m(x).logits


def check_files(path):
    """Structure check: .onnx + external .onnx.data exist, graph is valid, I/O is 1x3x224x224 -> 1x1000."""
    assert os.path.getsize(path) > 0, path
    assert os.path.getsize(path + ".data") > 0, "missing external weights: " + path + ".data"
    g = onnx.load(path, load_external_data=False).graph
    dims = lambda v: [d.dim_value for d in v.type.tensor_type.shape.dim]
    assert [i.name for i in g.input] == ["input"] and dims(g.input[0]) == [1, 3, 224, 224], g.input
    assert [o.name for o in g.output] == ["logits"] and dims(g.output[0]) == [1, 1000], g.output
    onnx.checker.check_model(path)  # path form, so large external data is allowed


def run_ort(path, x):
    return torch.from_numpy(ort.InferenceSession(path, providers=["CPUExecutionProvider"]).run(None, {"input": x.numpy()})[0])


def verify(m, path, x):
    check_files(path)
    with torch.no_grad():
        ref = m.cpu()(x)
    out = run_ort(path, x)
    diff = (ref - out).abs().max().item()
    same_top = ref.argmax().item() == out.argmax().item()
    print(f"max abs diff {diff:.2e}, top1 torch {ref.argmax().item()} / ort {out.argmax().item()}")
    assert diff < 1e-3 and same_top, "ONNX output differs from torch"  
    print("verify OK")


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "models"
    out = sys.argv[2] if len(sys.argv) > 2 else "models/resnet50.onnx"
    m = Logits(load(src)).eval()
    x = torch.randn(1, 3, 224, 224)
    torch.onnx.export(m, (x,), out, input_names=["input"], output_names=["logits"],
                      dynamo=True, opset_version=18)
    print("saved", out)
    verify(m, out, x)
