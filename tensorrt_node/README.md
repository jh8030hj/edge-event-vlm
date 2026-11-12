# tensorrt_node

Jetson Orin Nano에서 PyTorch 모델을 ONNX로 내보내 TensorRT engine으로 만드는 환경.

## 환경

보드에서 확인한 값 (2026-11-12).

| 항목 | 버전 |
|---|---|
| 보드 | Jetson Orin Nano (sm_87, CC 8.7, 8GB) |
| L4T | R39.2.1 (JetPack 7.2는 외부 자료 기준, 보드에서 미확인) |
| OS | Ubuntu 24.04.4, 커널 6.8.12-tegra |
| CUDA | 13.2 (nvcc 13.2.86) |
| cuDNN | 9.20.0 |
| TensorRT | 10.16.2 (apt, 시스템) |
| Python | 3.12.3 |
| torch / torchvision | 2.15.0 / 0.29.2 (+cu132, venv) |
| transformers / accelerate | 5.22.2 / 1.15.0 (venv) |
| onnx / onnxscript | 1.23.2 / 0.7.2 (venv) |

## 설치

```bash
# TensorRT가 보이도록 --system-site-packages 사용 (venv 패키지가 우선)
python3.12 -m venv ~/.venvs/torch --system-site-packages
source ~/.venvs/torch/bin/activate
python -m pip install --upgrade pip setuptools wheel
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu132
pip install transformers accelerate onnx onnxscript   # HF 모델 로드 / ONNX 내보내기용
```

활성화: `source ~/.venvs/torch/bin/activate`, 비활성화: `deactivate`

## 동작 확인

아래 코드를 실행한 결과: `2.15.0+cu132`, CUDA 13.2, `is_available() True`, GPU `Orin`, NaN `False`.

```python
import torch, tensorrt
print(torch.__version__, torch.version.cuda, torch.cuda.is_available(), tensorrt.__version__)
x = torch.randn(1024, 1024, device="cuda")
print(torch.isnan(x @ x).any().item())   # False면 정상
```

## 모델 (Francesco/resnet50-224-1k)

- `resnet_test/`: HF에서 받은 `pytorch_model.bin`, `config.json`, `preprocessor_config.json` (`.bin`은 git 제외).
- `load_resnet.py`: 구형(transformers 4.17) 체크포인트를 v5에 맞게 키/config를 바꿔 `strict=True`로 로드한다.
  `python load_resnet.py resnet_test`
- 전처리: 256 resize(bicubic) 후 224 center crop, mean `0.485 0.456 0.406`, std `0.229 0.224 0.225`.

## 참고

- `Found GPU0 Orin which is of compute capability (CC) 8.7` 경고는 무해하다.
  끄려면 `warnings.filterwarnings("ignore", message=".*Found GPU.*compute capability.*")`.
- `pypi.jetson-ai-lab.io/jp6/*`와 NVIDIA `jp/v61` 휠은 JetPack 6용이므로 쓰지 않는다.
- jetson-containers에 r39 이미지는 아직 없다. 컨테이너가 필요하면
  `nvcr.io/nvidia/cuda:13.0.0-devel-ubuntu24.04`를 베이스로 쓴다.
- jtop의 "Jetpack NOT DETECTED"는 표시 문제일 뿐이다.
