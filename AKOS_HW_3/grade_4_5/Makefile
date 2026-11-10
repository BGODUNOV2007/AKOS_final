CC ?= gcc
CFLAGS ?= -Wall -Wextra -pedantic -std=c11 -O2 -I../common
LDFLAGS ?= -lrt -lpthread

SRCS = main.c ../common/utils.c ../common/config.c
OBJS = $(SRCS:.c=.o)
TARGET = readers_writers_4_5

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o ../common/*.o *.log

.PHONY: all clean
