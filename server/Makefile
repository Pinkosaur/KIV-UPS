CC = gcc
CFLAGS = -pthread
BIN = server
OBJ = server.o

%.o: %c
	$(CC) -c $(CFLAGS) $< -o $@

$(BIN): $(OBJ)
	$(CC) $^ -o $@