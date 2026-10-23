CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g -O2
LDFLAGS = -lpthread

TARGET  = rw_sem
SRCS    = main.c database.c reader.c writer.c utils.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean run run-readers run-writers run-fair run-inf

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET) -p fair -r 3 -w 2 -o 5

run-readers: $(TARGET)
	./$(TARGET) -p readers -r 5 -w 1 -o 4 --sleep-min 300 --sleep-max 800

run-writers: $(TARGET)
	./$(TARGET) -p writers -r 2 -w 4 -o 4 --sleep-min 300 --sleep-max 800

run-fair: $(TARGET)
	./$(TARGET) -p fair -r 4 -w 2 -o 5

run-inf: $(TARGET)
	./$(TARGET) -p fair -r 3 -w 2 -o 0

run-log: $(TARGET)
	./$(TARGET) -p fair -r 3 -w 2 -o 5 --log run.log
