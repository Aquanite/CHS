#ifndef CHS_ARCH_H
#define CHS_ARCH_H

#include "chs/assembler.h"

typedef struct {
    const char *mnemonic;
    const char *operands;
    uint32_t encoded;
    uint8_t bytes[16];
    uint8_t size;
    uint8_t fill_byte;
    bool uses_symbol;
    const char *symbol_name;
    ChsRelocationKind relocation_kind;
    bool pc_relative;
    uint8_t relocation_offset;
    int64_t relocation_addend;
} ChsEncodedInstruction;

typedef struct ChsArchOps {
    ChsArchKind kind;
    const char *name;
    uint8_t pointer_size;
    uint8_t instruction_slot_size;
    bool (*encode_instruction)(const ChsObject *object,
                               const ChsSection *section,
                               uint64_t section_offset,
                               const char *mnemonic,
                               const char *operands,
                               ChsEncodedInstruction *encoded,
                               ChsError *error);
} ChsArchOps;

const ChsArchOps *chs_find_arch_ops(ChsArchKind kind);

#endif
