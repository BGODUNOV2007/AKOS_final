CC ?= gcc
CFLAGS ?= -Wall -Wextra -pedantic -std=c11 -O2 -I../common
LDFLAGS ?= -lrt -lpthread

COMMON_OBJS = ../common/utils.o ../common/config.o
TARGETS = coordinator reader writer

all: $(COMMON_OBJS) $(TARGETS)

../common/%.o: ../common/%.c
	$(CC) $(CFLAGS) -c $< -o $@

coordinator: coordinator.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

reader: reader.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

writer: writer.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGETS) *.o ../common/*.o *.log

.PHONY: all clean
