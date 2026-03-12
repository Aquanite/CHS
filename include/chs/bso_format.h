#ifndef BSO_FORMAT_H
#define BSO_FORMAT_H

#include <stdint.h>

#define BSO_MAGIC "BSO1"
#define BSO_MAGIC_SIZE 4
#define BSO_VERSION 1

typedef enum {
    BSO_RELOC_ABS8 = 1,
    BSO_RELOC_ABS16 = 2,
    BSO_RELOC_ABS32 = 3,
    BSO_RELOC_ABS64 = 4,
    BSO_RELOC_REL8 = 5,
    BSO_RELOC_REL32 = 6
} bso_reloc_kind_t;

typedef struct {
    char magic[BSO_MAGIC_SIZE];
    uint32_t version;
    uint32_t code_size;
    uint32_t symbol_count;
    uint32_t relocation_count;
    uint32_t string_table_size;
} bso_header_t;

typedef struct {
    uint32_t name_offset;
    uint32_t value;
    uint32_t flags;
} bso_symbol_record_t;

#define BSO_SYMBOL_DEFINED (1u << 0)
#define BSO_SYMBOL_GLOBAL  (1u << 1)

typedef struct {
    uint32_t offset;
    int32_t addend;
    uint32_t symbol_index;
    uint32_t kind;
} bso_relocation_record_t;

#endif /* BSO_FORMAT_H */
