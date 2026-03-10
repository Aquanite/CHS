#ifndef CHS_OBJECT_H
#define CHS_OBJECT_H

#include "chs/assembler.h"

bool chs_object_get_or_create_section(ChsObject *object,
                                      const char *segment_name,
                                      const char *section_name,
                                      uint32_t macho_flags,
                                      size_t *section_index,
                                      ChsError *error);
bool chs_object_get_or_create_symbol(ChsObject *object,
                                     const char *name,
                                     size_t *symbol_index,
                                     ChsError *error);
bool chs_section_append_data(ChsSection *section, const void *data, size_t size, ChsError *error);
bool chs_section_append_zeros(ChsSection *section, size_t size, ChsError *error);
bool chs_section_align(ChsSection *section, uint64_t alignment, ChsError *error);
bool chs_section_add_relocation(ChsSection *section,
                                uint64_t offset,
                                ChsRelocationKind kind,
                                size_t symbol_index,
                                int64_t addend,
                                bool pc_relative,
                                ChsError *error);
const ChsSymbol *chs_object_find_symbol(const ChsObject *object, const char *name, size_t *symbol_index);

bool chs_write_macho_object(const ChsObject *object, const char *output_path, ChsError *error);
bool chs_write_elf64_object(const ChsObject *object, const char *output_path, ChsError *error);

#endif
