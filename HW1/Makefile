CC       = gcc
CFLAGS   = -Wall -Wextra -Wpedantic -std=c11 -g -O2
LDFLAGS  = -lpthread

TARGET   = readers_writers
SRCS     = main.c database.c reader.c writer.c utils.c
OBJS     = $(SRCS:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)

run-readers: $(TARGET)
	./$(TARGET) -p readers -r 5 -w 2 -o 3

run-writers: $(TARGET)
	./$(TARGET) -p writers -r 3 -w 3 -o 3

run-fair: $(TARGET)
	./$(TARGET) -p fair -r 4 -w 2 -o 4
