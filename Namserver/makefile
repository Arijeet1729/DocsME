CC = gcc
CFLAGS = -pthread  -O2

TARGET = nameserver
SRCS = nameserver.c nm_storage.c nm_files.c logger.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c nm.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(TARGET) $(OBJS)
