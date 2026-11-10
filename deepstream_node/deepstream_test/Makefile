CC     = gcc
CFLAGS = -Wall $(shell pkg-config --cflags gstreamer-1.0)
LIBS   = $(shell pkg-config --libs gstreamer-1.0)

DS = /opt/nvidia/deepstream/deepstream

all: bin/capture_imx219 bin/stream_count bin/infer_stream

bin/infer_stream: CFLAGS += -I$(DS)/includes -DPLATFORM_TEGRA
bin/infer_stream: LIBS += -L$(DS)/lib -lnvdsgst_meta -lnvds_meta -Wl,-rpath,$(DS)/lib

bin/%: src/%.c src/common.h | bin
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LIBS)

bin:
	mkdir -p $@

clean:
	rm -rf bin

.PHONY: all clean
