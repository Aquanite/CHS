CC := clang
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude
LDFLAGS :=
BUILD_DIR := build
PREFIX ?= /usr/local
BIN_DIR := $(PREFIX)/bin
EXE_SUFFIX :=
BIN_NAME := chs

ifeq ($(OS),Windows_NT)
EXE_SUFFIX := .exe
BIN_NAME := chs.exe
endif

TARGET := $(BUILD_DIR)/chs$(EXE_SUFFIX)
SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all clean test install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@-mkdir "$(BUILD_DIR)"
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@-mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	-rm -rf "$(BUILD_DIR)"
	-rmdir /S /Q "$(BUILD_DIR)"

test: $(TARGET)
	./tests/run.sh

install: $(TARGET)
ifeq ($(OS),Windows_NT)
	@-mkdir "$(BIN_DIR)"
	copy /Y "$(TARGET)" "$(BIN_DIR)\\$(BIN_NAME)" >nul
else
	@mkdir -p "$(BIN_DIR)"
	install -m 755 "$(TARGET)" "$(BIN_DIR)/$(BIN_NAME)"
endif

uninstall:
ifeq ($(OS),Windows_NT)
	-del /Q "$(BIN_DIR)\\$(BIN_NAME)"
else
	rm -f "$(BIN_DIR)/$(BIN_NAME)"
endif
