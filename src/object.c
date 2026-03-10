#include "chs/object.h"

#include <stdlib.h>
#include <string.h>

static bool chs_object_reserve_sections(ChsObject *object, ChsError *error) {
    size_t new_capacity;
    ChsSection *new_sections;

    if (object->section_count < object->section_capacity) {
        return true;
    }

    new_capacity = object->section_capacity == 0 ? 8 : object->section_capacity * 2;
    new_sections = realloc(object->sections, new_capacity * sizeof(*new_sections));
    if (new_sections == NULL) {
        chs_set_error(error, "out of memory growing section table");
        return false;
    }

    memset(new_sections + object->section_capacity, 0,
           (new_capacity - object->section_capacity) * sizeof(*new_sections));
    object->sections = new_sections;
    object->section_capacity = new_capacity;
    return true;
}


static bool chs_object_reserve_symbols(ChsObject *object, ChsError *error) {
    size_t new_capacity;
    ChsSymbol *new_symbols;

    if (object->symbol_count < object->symbol_capacity) {
        return true;
    }

    new_capacity = object->symbol_capacity == 0 ? 16 : object->symbol_capacity * 2;
    new_symbols = realloc(object->symbols, new_capacity * sizeof(*new_symbols));
    if (new_symbols == NULL) {
        chs_set_error(error, "out of memory growing symbol table");
        return false;
    }

    memset(new_symbols + object->symbol_capacity, 0,
           (new_capacity - object->symbol_capacity) * sizeof(*new_symbols));
    object->symbols = new_symbols;
    object->symbol_capacity = new_capacity;
    return true;
}

bool chs_object_get_or_create_section(ChsObject *object,
                                      const char *segment_name,
                                      const char *section_name,
                                      uint32_t macho_flags,
                                      size_t *section_index,
                                      ChsError *error) {
    size_t index;

    for (index = 0; index < object->section_count; ++index) {
        if (strcmp(object->sections[index].name, section_name) == 0 &&
            strcmp(object->sections[index].segment_name, segment_name) == 0) {
            object->sections[index].macho_flags = macho_flags;
            *section_index = index;
            return true;
        }
    }

    if (!chs_object_reserve_sections(object, error)) {
        return false;
    }

    index = object->section_count++;
    object->sections[index].name = chs_strdup(section_name, error);
    if (object->sections[index].name == NULL) {
        return false;
    }
    object->sections[index].segment_name = chs_strdup(segment_name, error);
    if (object->sections[index].segment_name == NULL) {
        return false;
    }
    object->sections[index].macho_flags = macho_flags;
    object->sections[index].align = 1;
    *section_index = index;
    return true;
}

bool chs_object_get_or_create_symbol(ChsObject *object,
                                     const char *name,
                                     size_t *symbol_index,
                                     ChsError *error) {
    const ChsSymbol *existing;

    existing = chs_object_find_symbol(object, name, symbol_index);
    if (existing != NULL) {
        return true;
    }

    if (!chs_object_reserve_symbols(object, error)) {
        return false;
    }

    *symbol_index = object->symbol_count++;
    object->symbols[*symbol_index].name = chs_strdup(name, error);
    if (object->symbols[*symbol_index].name == NULL) {
        return false;
    }
    return true;
}

bool chs_section_append_data(ChsSection *section, const void *data, size_t size, ChsError *error) {
    size_t new_capacity;
    uint8_t *new_data;

    if (section->size + size > section->capacity) {
        new_capacity = section->capacity == 0 ? 64 : section->capacity;
        while (new_capacity < section->size + size) {
            new_capacity *= 2;
        }
        new_data = realloc(section->data, new_capacity);
        if (new_data == NULL) {
            chs_set_error(error, "out of memory growing section %s", section->name);
            return false;
        }
        section->data = new_data;
        section->capacity = new_capacity;
    }

    memcpy(section->data + section->size, data, size);
    section->size += size;
    return true;
}

bool chs_section_append_zeros(ChsSection *section, size_t size, ChsError *error) {
    size_t start;

    start = section->size;
    if (!chs_section_append_data(section, "", 1, error) && size != 0) {
        return false;
    }
    section->size = start;
    if (section->size + size > section->capacity) {
        size_t new_capacity = section->capacity == 0 ? 64 : section->capacity;
        uint8_t *new_data;

        while (new_capacity < section->size + size) {
            new_capacity *= 2;
        }
        new_data = realloc(section->data, new_capacity);
        if (new_data == NULL) {
            chs_set_error(error, "out of memory growing section %s", section->name);
            return false;
        }
        section->data = new_data;
        section->capacity = new_capacity;
    }

    memset(section->data + section->size, 0, size);
    section->size += size;
    return true;
}

bool chs_section_align(ChsSection *section, uint64_t alignment, ChsError *error) {
    uint64_t aligned_size;

    if (alignment == 0) {
        alignment = 1;
    }
    if (alignment > section->align) {
        section->align = alignment;
    }

    aligned_size = chs_align_up_u64((uint64_t) section->size, alignment);
    if (aligned_size == (uint64_t) section->size) {
        return true;
    }
    return chs_section_append_zeros(section, (size_t) (aligned_size - section->size), error);
}

bool chs_section_add_relocation(ChsSection *section,
                                uint64_t offset,
                                ChsRelocationKind kind,
                                size_t symbol_index,
                                int64_t addend,
                                bool pc_relative,
                                ChsError *error) {
    size_t new_capacity;
    ChsRelocation *new_relocations;

    if (section->relocation_count == section->relocation_capacity) {
        new_capacity = section->relocation_capacity == 0 ? 8 : section->relocation_capacity * 2;
        new_relocations = realloc(section->relocations, new_capacity * sizeof(*new_relocations));
        if (new_relocations == NULL) {
            chs_set_error(error, "out of memory growing relocation table");
            return false;
        }
        section->relocations = new_relocations;
        section->relocation_capacity = new_capacity;
    }

    section->relocations[section->relocation_count].offset = offset;
    section->relocations[section->relocation_count].kind = kind;
    section->relocations[section->relocation_count].symbol_index = symbol_index;
    section->relocations[section->relocation_count].addend = addend;
    section->relocations[section->relocation_count].pc_relative = pc_relative;
    ++section->relocation_count;
    return true;
}

const ChsSymbol *chs_object_find_symbol(const ChsObject *object, const char *name, size_t *symbol_index) {
    size_t index;

    for (index = 0; index < object->symbol_count; ++index) {
        if (strcmp(object->symbols[index].name, name) == 0) {
            if (symbol_index != NULL) {
                *symbol_index = index;
            }
            return &object->symbols[index];
        }
    }
    return NULL;
}

void chs_object_free(ChsObject *object) {
    size_t section_index;
    size_t symbol_index;

    for (section_index = 0; section_index < object->section_count; ++section_index) {
        free(object->sections[section_index].name);
        free(object->sections[section_index].segment_name);
        free(object->sections[section_index].data);
        free(object->sections[section_index].relocations);
    }
    for (symbol_index = 0; symbol_index < object->symbol_count; ++symbol_index) {
        free(object->symbols[symbol_index].name);
    }

    free(object->sections);
    free(object->symbols);
    memset(object, 0, sizeof(*object));
}