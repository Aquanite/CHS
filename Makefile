CC := clang
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude
LDFLAGS :=
BUILD_DIR := build
TARGET := $(BUILD_DIR)/chs
TARGET_EXE := $(TARGET).exe
PREFIX ?= /usr/local
BIN_DIR := $(PREFIX)/bin
BIN_NAME := chs

ifeq ($(OS),Windows_NT)
BIN_NAME := chs.exe
endif

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
	@if [ -f "$(TARGET)" ]; then \
		install -m 755 "$(TARGET)" "$(BIN_DIR)/$(BIN_NAME)"; \
	elif [ -f "$(TARGET_EXE)" ]; then \
		install -m 755 "$(TARGET_EXE)" "$(BIN_DIR)/$(BIN_NAME)"; \
	else \
		echo "missing build target: $(TARGET) or $(TARGET_EXE)"; \
		exit 1; \
	fi

uninstall:
	rm -f "$(BIN_DIR)/$(BIN_NAME)"