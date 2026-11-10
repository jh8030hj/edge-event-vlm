# deepstream_node

DeepStream pipeline project on Jetson Orin Nano.

## Structure
```
deepstream_node/
└── deepstream_test/   # DeepStream pipeline experiments
    ├── src/           # C sources (one file per pipeline)
    ├── config/        # nvinfer config + labels
    ├── models/        # TensorRT engines (git-ignored)
    ├── bin/           # built binaries (git-ignored)
    ├── output/        # captured images (git-ignored)
    └── Makefile
```

## Build and run
```
cd deepstream_test
make
./bin/capture_imx219 [output/name.jpg]   # one frame
./bin/stream_count [seconds] [out.jpg]    # stream, stop after N seconds, save last frame
./bin/infer_stream [seconds] [config]   # classify with a TensorRT engine, print top class each second
```

## Classifier engine (torchvision resnet18)
Export with softmax (so confidence is 0-1) and build on this Jetson:
```
python3 -c "import torch,torchvision as tv; m=tv.models.resnet18(weights='DEFAULT'); \
torch.onnx.export(torch.nn.Sequential(m,torch.nn.Softmax(dim=1)).eval(), torch.zeros(1,3,224,224), 'models/resnet18.onnx', input_names=['input'], \
 dynamic_axes={'input':{0:'batch'}}, dynamo=False)"
trtexec --onnx=models/resnet18.onnx --saveEngine=models/resnet18.engine --fp16 \
  --minShapes=input:1x3x224x224 --optShapes=input:1x3x224x224 --maxShapes=input:2x3x224x224
```
The batch dimension must be dynamic (max > min): a static engine is read as implicit-batch (`layers num: 0`) and nvinfer returns no results.
`config/labels.txt` is one line, labels separated by `;` (one line per output layer, not per class).

Another model: edit `config/model.txt` (engine path, preprocessing, `network-mode`) and `config/labels.txt`.
