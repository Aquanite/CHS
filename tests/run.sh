#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

rm -f build/sample_macho.o build/sample_elf.o build/bslash.bin build/bslash.bas.bin build/bslash_elf.o build/bslash_macho.o
./build/chs --arch arm64 --format macho --output build/sample_macho.o samples/chance_arm64_macos.s
./build/chs --arch arm64 --format elf64 --output build/sample_elf.o samples/chance_arm64_macos.s
./build/chs --arch bslash --format bin --output build/bslash.bin tests/bslash_flat.bas
../../BSlash/bas/bas -o build/bslash.bas.bin tests/bslash_flat.bas
./build/chs --arch bslash --format elf64 --output build/bslash_elf.o tests/bslash_object.bas
./build/chs --arch bslash --format macho --output build/bslash_macho.o tests/bslash_object.bas

test -s build/sample_macho.o
test -s build/sample_elf.o
cmp -s build/bslash.bin build/bslash.bas.bin

python3 - <<'PY'
from pathlib import Path

def expect_prefix(path: str, prefix: bytes) -> None:
	data = Path(path).read_bytes()
	if not data.startswith(prefix):
		raise SystemExit(f"{path} does not start with {prefix.hex()}")

expect_prefix("build/bslash_elf.o", b"\x7fELF")
expect_prefix("build/bslash_macho.o", bytes.fromhex("cffaedfe"))
PY

strings build/bslash_elf.o | grep -q '^extern_call$'
strings build/bslash_elf.o | grep -q '^extern_qword$'
strings build/bslash_macho.o | grep -q '^extern_call$'
strings build/bslash_macho.o | grep -q '^extern_qword$'
