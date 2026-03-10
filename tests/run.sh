#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

rm -f build/sample_macho.o build/sample_elf.o
./build/chs --arch arm64 --format macho --output build/sample_macho.o samples/chance_arm64_macos.s
./build/chs --arch arm64 --format elf64 --output build/sample_elf.o samples/chance_arm64_macos.s

test -s build/sample_macho.o
test -s build/sample_elf.o