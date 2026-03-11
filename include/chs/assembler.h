#ifndef CHS_ASSEMBLER_H
#define CHS_ASSEMBLER_H

#include "chs/common.h"

typedef enum {
    CHS_ARCH_ARM64,
    CHS_ARCH_X86_64,
    CHS_ARCH_BSLASH
} ChsArchKind;

typedef enum {
    CHS_OUTPUT_MACHO,
    CHS_OUTPUT_ELF64,
    CHS_OUTPUT_BIN
} ChsOutputKind;

typedef enum {
    CHS_RELOC_AARCH64_BRANCH26,
    CHS_RELOC_AARCH64_PAGE21,
    CHS_RELOC_AARCH64_PAGEOFF12,
    CHS_RELOC_BSLASH_ABS8,
    CHS_RELOC_BSLASH_ABS16,
    CHS_RELOC_BSLASH_ABS32,
    CHS_RELOC_BSLASH_ABS64,
    CHS_RELOC_BSLASH_REL8,
    CHS_RELOC_BSLASH_REL32
} ChsRelocationKind;

typedef struct {
    uint64_t offset;
    ChsRelocationKind kind;
    size_t symbol_index;
    int64_t addend;
    bool pc_relative;
} ChsRelocation;

typedef struct {
    char *name;
    char *segment_name;
    uint32_t macho_flags;
    uint64_t align;
    uint8_t *data;
    size_t size;
    size_t capacity;
    ChsRelocation *relocations;
    size_t relocation_count;
    size_t relocation_capacity;
} ChsSection;

typedef struct {
    char *name;
    bool defined;
    bool global_binding;
    bool external_reference;
    size_t section_index;
    uint64_t value;
} ChsSymbol;

typedef struct {
    ChsArchKind arch;
    ChsOutputKind output_kind;
    ChsSection *sections;
    size_t section_count;
    size_t section_capacity;
    ChsSymbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    unsigned build_version_major;
    unsigned build_version_minor;
    unsigned build_version_patch;
    bool has_build_version;
} ChsObject;

typedef struct {
    const char *input_path;
    const char *output_path;
    ChsArchKind arch;
    ChsOutputKind output_kind;
} ChsAssembleOptions;

bool chs_assemble_file(const ChsAssembleOptions *options, ChsError *error);
void chs_object_free(ChsObject *object);

#endif
