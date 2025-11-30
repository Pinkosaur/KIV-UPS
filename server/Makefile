CC = gcc
CFLAGS = -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -g -Iinclude
LDFLAGS = -pthread

SRCS = src/main.c src/client.c src/match.c src/game.c src/logging.c
OBJS = $(SRCS:.c=.o)
TARGET = server.exe

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
