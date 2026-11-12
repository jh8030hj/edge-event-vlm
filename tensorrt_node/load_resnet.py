"""Load the old-format ResNet-50 checkpoint into transformers 5 (key + config fix)."""
import json
import sys

import torch
from transformers import ResNetConfig, ResNetForImageClassification


def load(d):
    cfg = json.load(open(f"{d}/config.json"))
    # old config lists the 64-ch stem as hidden_sizes[0]; v5 wants it as embedding_size
    cfg["embedding_size"], *cfg["hidden_sizes"] = cfg["hidden_sizes"]
    model = ResNetForImageClassification(ResNetConfig(**cfg))

    sd = torch.load(f"{d}/pytorch_model.bin", map_location="cpu", weights_only=True)
    sd = {
        k.replace("model.", "resnet.", 1)
        .replace(".conv.", ".convolution.")
        .replace(".bn.", ".normalization."): v
        for k, v in sd.items()
    }
    model.load_state_dict(sd, strict=True)  # raises on any missing/unexpected key
    return model.eval()


if __name__ == "__main__":
    m = load(sys.argv[1] if len(sys.argv) > 1 else "resnet_test").cuda()
    x = torch.randn(1, 3, 224, 224, device="cuda")
    with torch.no_grad():
        y = m(x).logits
    print("logits", tuple(y.shape), "nan", torch.isnan(y).any().item())
