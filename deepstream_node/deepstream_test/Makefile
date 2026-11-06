CC     = gcc
CFLAGS = -Wall $(shell pkg-config --cflags gstreamer-1.0)
LIBS   = $(shell pkg-config --libs gstreamer-1.0)

all: bin/capture_imx219 bin/stream_count

bin/%: src/%.c | bin
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LIBS)

bin:
	mkdir -p $@

clean:
	rm -rf bin

.PHONY: all clean
