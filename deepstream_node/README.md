# deepstream_node

DeepStream pipeline project on Jetson Orin Nano.

## Structure
```
deepstream_node/
└── deepstream_test/   # DeepStream pipeline experiments
    ├── src/           # C sources (one file per pipeline)
    ├── bin/           # built binaries (git-ignored)
    ├── output/        # captured images (git-ignored)
    └── Makefile
```

## Build and run
```
cd deepstream_test
make
./bin/capture_imx219 [output/name.jpg]
```
