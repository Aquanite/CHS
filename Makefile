CC := clang
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude
LDFLAGS :=
BUILD_DIR := build
TARGET := $(BUILD_DIR)/chs
PREFIX ?= /usr/local
BIN_DIR := $(PREFIX)/bin
SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all clean test install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

test: $(TARGET)
	./tests/run.sh

install: $(TARGET)
	@mkdir -p $(BIN_DIR)
	install -m 755 $(TARGET) $(BIN_DIR)/chs

uninstall:
	rm -f $(BIN_DIR)/chs