CC = cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
BIN = bin/server bin/reader bin/writer bin/monitor bin/control
HDR = src/common.h src/config.h

.PHONY: all test demo clean
all: $(BIN)

bin:
	mkdir -p $@

bin/server: src/server.c src/config.c src/common.c $(HDR) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

bin/reader bin/writer: src/worker.c src/common.c $(HDR) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -DROLE="'$(if $(filter bin/reader,$@),R,W)'" $(filter %.c,$^) $(LDFLAGS) -o $@

bin/monitor bin/control: bin/%: src/%.c src/common.c $(HDR) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@

test: all
	python3 tests/integration.py

demo: all
	bash scripts/demo.sh

clean:
	$(RM) $(BIN)
