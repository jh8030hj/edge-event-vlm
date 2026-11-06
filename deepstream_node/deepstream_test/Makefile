CC     = gcc
CFLAGS = -Wall $(shell pkg-config --cflags gstreamer-1.0)
LIBS   = $(shell pkg-config --libs gstreamer-1.0)

bin/capture_imx219: src/capture_imx219.c | bin
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)

bin:
	mkdir -p $@

clean:
	rm -rf bin

.PHONY: clean
