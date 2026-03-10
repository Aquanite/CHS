#ifndef CHS_ARCH_H
#define CHS_ARCH_H

#include "chs/assembler.h"

typedef struct {
    const char *mnemonic;
    const char *operands;
    uint32_t encoded;
    bool uses_symbol;
    const char *symbol_name;
    ChsRelocationKind relocation_kind;
    bool pc_relative;
} ChsEncodedInstruction;

typedef struct ChsArchOps {
    ChsArchKind kind;
    const char *name;
    uint8_t pointer_size;
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
