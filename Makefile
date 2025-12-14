# Makefile for DEI Emergency System

CC = gcc

CFLAGS = -std=c11 -Wall -Wextra -O2

LDLIBS = -pthread

SRC = main.c
OBJ = $(SRC:.c=.o)
TARGET = dei_emergency
CLIENT_SRC = client.c
CLIENT_TARGET = client

.PHONY: all run clean debug format rebuild

all: $(TARGET) $(CLIENT_TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) $(LDFLAGS)

$(CLIENT_TARGET): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Run the application (expects config.txt in working directory)
run: $(TARGET)
	./$(TARGET)

# Debug build (no optimization + debug symbols)
debug: CFLAGS = -std=c11 -Wall -Wextra -O0 -g
debug: clean $(TARGET)

# Format source (optional; requires clang-format installed)
format:
	clang-format -i $(SRC) || echo "clang-format not installed; skipping format"

# Rebuild from scratch
rebuild: clean all

clean:
	$(RM) $(OBJ) $(TARGET)
