#include "chs/object.h"

#include <stdlib.h>
#include <string.h>

#define CHS_MH_MAGIC_64 0xfeedfacfu
#define CHS_CPU_TYPE_ARM64 0x0100000cu
#define CHS_CPU_SUBTYPE_ARM64_ALL 0u
#define CHS_CPU_TYPE_X86_64 0x01000007u
#define CHS_CPU_SUBTYPE_X86_64_ALL 3u
#define CHS_CPU_TYPE_BSLASH 0x01004253u
#define CHS_CPU_SUBTYPE_BSLASH_ALL 0u
#define CHS_MH_OBJECT 0x1u
#define CHS_LC_SEGMENT_64 0x19u
#define CHS_LC_SYMTAB 0x2u
#define CHS_LC_DYSYMTAB 0xbu
#define CHS_LC_BUILD_VERSION 0x32u
#define CHS_PLATFORM_MACOS 1u

#define CHS_ARM64_RELOC_UNSIGNED 0u
#define CHS_ARM64_RELOC_BRANCH26 2u
#define CHS_ARM64_RELOC_PAGE21 3u
#define CHS_ARM64_RELOC_PAGEOFF12 4u

#define CHS_X86_64_RELOC_UNSIGNED 0u
#define CHS_X86_64_RELOC_SIGNED 1u
#define CHS_X86_64_RELOC_BRANCH 2u

#define CHS_BSLASH_RELOC_ABS8 0u
#define CHS_BSLASH_RELOC_ABS16 1u
#define CHS_BSLASH_RELOC_ABS32 2u
#define CHS_BSLASH_RELOC_ABS64 3u
#define CHS_BSLASH_RELOC_REL8 4u
#define CHS_BSLASH_RELOC_REL32 5u

typedef struct {
    uint32_t string_index;
    uint8_t type;
    uint8_t section_number;
    uint16_t description;
    uint64_t value;
} ChsMachOSymbol;

typedef struct {
    size_t object_symbol_index;
    bool undefined_symbol;
    bool external_symbol;
} ChsMachOSortEntry;

static unsigned chs_macho_log2_align(uint64_t alignment) {
    unsigned power;

    power = 0;
    while ((1ull << power) < alignment && power < 15u) {
        ++power;
    }
    return power;
}

static bool chs_macho_arch_info(const ChsObject *object,
                                uint32_t *cpu_type,
                                uint32_t *cpu_subtype,
                                ChsError *error) {
    switch (object->arch) {
        case CHS_ARCH_ARM64:
            *cpu_type = CHS_CPU_TYPE_ARM64;
            *cpu_subtype = CHS_CPU_SUBTYPE_ARM64_ALL;
            return true;
        case CHS_ARCH_BSLASH:
            *cpu_type = CHS_CPU_TYPE_BSLASH;
            *cpu_subtype = CHS_CPU_SUBTYPE_BSLASH_ALL;
            return true;
        case CHS_ARCH_X86_64:
            *cpu_type = CHS_CPU_TYPE_X86_64;
            *cpu_subtype = CHS_CPU_SUBTYPE_X86_64_ALL;
            return true;
    }

    chs_set_error(error, "Mach-O writer currently supports ARM64, x86_64, and BSlash only");
    return false;
}

static unsigned chs_macho_relocation_width(const ChsRelocation *relocation) {
    switch (relocation->kind) {
        case CHS_RELOC_AARCH64_BRANCH26:
        case CHS_RELOC_AARCH64_PAGE21:
        case CHS_RELOC_AARCH64_PAGEOFF12:
        case CHS_RELOC_X86_64_BRANCH32:
        case CHS_RELOC_X86_64_SIGNED32:
        case CHS_RELOC_X86_64_ABS32:
        case CHS_RELOC_BSLASH_ABS32:
        case CHS_RELOC_BSLASH_REL32:
            return 2u;
        case CHS_RELOC_BSLASH_ABS8:
        case CHS_RELOC_BSLASH_REL8:
            return 0u;
        case CHS_RELOC_BSLASH_ABS16:
            return 1u;
        case CHS_RELOC_AARCH64_ABS64:
        case CHS_RELOC_X86_64_ABS64:
        case CHS_RELOC_BSLASH_ABS64:
            return 3u;
    }

    return 2u;
}


static int chs_macho_symbol_rank(const ChsSymbol *symbol) {
    if (!symbol->defined) {
        return 2;
    }
    if (symbol->global_binding || symbol->external_reference) {
        return 1;
    }
    return 0;
}

static bool chs_macho_append_fixed_string(ChsBuffer *buffer, const char *text, size_t width, ChsError *error) {
    size_t length;
    char fixed[16];

    memset(fixed, 0, sizeof(fixed));
    length = strlen(text);
    if (length > width) {
        length = width;
    }
    memcpy(fixed, text, length);
    return chs_buffer_append(buffer, fixed, width, error);
}

static bool chs_macho_append_symbol_table(ChsBuffer *buffer,
                                          const ChsMachOSymbol *symbols,
                                          size_t symbol_count,
                                          ChsError *error) {
    size_t index;

    for (index = 0; index < symbol_count; ++index) {
        if (!chs_buffer_append_u32le(buffer, symbols[index].string_index, error) ||
            !chs_buffer_append_u8(buffer, symbols[index].type, error) ||
            !chs_buffer_append_u8(buffer, symbols[index].section_number, error) ||
            !chs_buffer_append_u16le(buffer, symbols[index].description, error) ||
            !chs_buffer_append_u64le(buffer, symbols[index].value, error)) {
            return false;
        }
    }
    return true;
}

static bool chs_macho_append_relocations(ChsBuffer *buffer,
                                         const ChsSection *section,
                                         const uint32_t *symbol_index_map,
                                         ChsError *error) {
    size_t index;

    for (index = 0; index < section->relocation_count; ++index) {
        uint32_t relocation_type;
        uint32_t packed;

        switch (section->relocations[index].kind) {
            case CHS_RELOC_AARCH64_BRANCH26:
                relocation_type = CHS_ARM64_RELOC_BRANCH26;
                break;
            case CHS_RELOC_AARCH64_PAGE21:
                relocation_type = CHS_ARM64_RELOC_PAGE21;
                break;
            case CHS_RELOC_AARCH64_PAGEOFF12:
                relocation_type = CHS_ARM64_RELOC_PAGEOFF12;
                break;
            case CHS_RELOC_AARCH64_ABS64:
                relocation_type = CHS_ARM64_RELOC_UNSIGNED;
                break;
            case CHS_RELOC_X86_64_BRANCH32:
                relocation_type = CHS_X86_64_RELOC_BRANCH;
                break;
            case CHS_RELOC_X86_64_SIGNED32:
                relocation_type = CHS_X86_64_RELOC_SIGNED;
                break;
            case CHS_RELOC_X86_64_ABS32:
            case CHS_RELOC_X86_64_ABS64:
                relocation_type = CHS_X86_64_RELOC_UNSIGNED;
                break;
            case CHS_RELOC_BSLASH_ABS8:
                relocation_type = CHS_BSLASH_RELOC_ABS8;
                break;
            case CHS_RELOC_BSLASH_ABS16:
                relocation_type = CHS_BSLASH_RELOC_ABS16;
                break;
            case CHS_RELOC_BSLASH_ABS32:
                relocation_type = CHS_BSLASH_RELOC_ABS32;
                break;
            case CHS_RELOC_BSLASH_ABS64:
                relocation_type = CHS_BSLASH_RELOC_ABS64;
                break;
            case CHS_RELOC_BSLASH_REL8:
                relocation_type = CHS_BSLASH_RELOC_REL8;
                break;
            case CHS_RELOC_BSLASH_REL32:
                relocation_type = CHS_BSLASH_RELOC_REL32;
                break;
        }

        packed = (symbol_index_map[section->relocations[index].symbol_index] & 0x00ffffffu) |
                 ((section->relocations[index].pc_relative ? 1u : 0u) << 24) |
                 (chs_macho_relocation_width(&section->relocations[index]) << 25) |
                 (1u << 27) |
                 (relocation_type << 28);
        if (!chs_buffer_append_u32le(buffer, (uint32_t) section->relocations[index].offset, error) ||
            !chs_buffer_append_u32le(buffer, packed, error)) {
            return false;
        }
    }
    return true;
}

bool chs_write_macho_object(const ChsObject *object, const char *output_path, ChsError *error) {
    ChsBuffer file_buffer;
    ChsBuffer string_table;
    ChsMachOSortEntry *sort_entries;
    ChsMachOSymbol *symbols;
    uint32_t *symbol_index_map;
    uint32_t sizeofcmds;
    uint32_t header_size;
    uint32_t load_command_size;
    uint32_t build_version_size;
    uint32_t section_data_offset;
    uint32_t relocation_offset;
    uint32_t symbol_table_offset;
    uint32_t string_table_offset;
    uint32_t cpu_type;
    uint32_t cpu_subtype;
    uint64_t total_section_size;
    size_t symbol_count;
    size_t index;
    size_t local_count;
    size_t extdef_count;
    size_t undef_count;
    bool success;

    memset(&file_buffer, 0, sizeof(file_buffer));
    memset(&string_table, 0, sizeof(string_table));
    sort_entries = NULL;
    symbols = NULL;
    symbol_index_map = NULL;
    success = false;

    if (!chs_macho_arch_info(object, &cpu_type, &cpu_subtype, error)) {
        goto cleanup;
    }

    symbol_count = object->symbol_count;
    sort_entries = calloc(symbol_count, sizeof(*sort_entries));
    symbols = calloc(symbol_count, sizeof(*symbols));
    symbol_index_map = calloc(symbol_count, sizeof(*symbol_index_map));
    if ((symbol_count != 0 && (sort_entries == NULL || symbols == NULL || symbol_index_map == NULL))) {
        chs_set_error(error, "out of memory allocating Mach-O symbol tables");
        goto cleanup;
    }

    for (index = 0; index < symbol_count; ++index) {
        sort_entries[index].object_symbol_index = index;
        sort_entries[index].undefined_symbol = !object->symbols[index].defined;
        sort_entries[index].external_symbol = object->symbols[index].global_binding || object->symbols[index].external_reference;
    }

    for (index = 0; index < symbol_count; ++index) {
        size_t rank_index;
        rank_index = index;
        while (rank_index > 0 &&
               chs_macho_symbol_rank(&object->symbols[sort_entries[rank_index - 1].object_symbol_index]) >
                   chs_macho_symbol_rank(&object->symbols[sort_entries[rank_index].object_symbol_index])) {
            ChsMachOSortEntry temporary = sort_entries[rank_index - 1];
            sort_entries[rank_index - 1] = sort_entries[rank_index];
            sort_entries[rank_index] = temporary;
            --rank_index;
        }
    }

    if (!chs_buffer_append_u8(&string_table, 0, error)) {
        goto cleanup;
    }

    local_count = 0;
    extdef_count = 0;
    undef_count = 0;
    for (index = 0; index < symbol_count; ++index) {
        const ChsSymbol *symbol;
        uint8_t type;
        uint8_t section_number;
        uint64_t value;
        uint32_t string_index;

        symbol = &object->symbols[sort_entries[index].object_symbol_index];
        symbol_index_map[sort_entries[index].object_symbol_index] = (uint32_t) index;
        string_index = (uint32_t) string_table.length;
        if (!chs_buffer_append(&string_table, symbol->name, strlen(symbol->name) + 1, error)) {
            goto cleanup;
        }
        if (!symbol->defined) {
            type = 0x01u;
            section_number = 0u;
            value = 0u;
            ++undef_count;
        } else {
            type = 0x0eu;
            section_number = (uint8_t) (symbol->section_index + 1);
            value = symbol->value;
            if (symbol->global_binding || symbol->external_reference) {
                type |= 0x01u;
                ++extdef_count;
            } else {
                ++local_count;
            }
        }
        symbols[index].string_index = string_index;
        symbols[index].type = type;
        symbols[index].section_number = section_number;
        symbols[index].description = 0;
        symbols[index].value = value;
    }

    build_version_size = object->has_build_version ? 24u : 0u;
    load_command_size = 72u + (uint32_t) (object->section_count * 80u);
    sizeofcmds = load_command_size + 24u + 80u + build_version_size;
    header_size = 32u;
    section_data_offset = header_size + sizeofcmds;
    total_section_size = 0;
    for (index = 0; index < object->section_count; ++index) {
        total_section_size += object->sections[index].size;
    }
    relocation_offset = section_data_offset + (uint32_t) total_section_size;
    symbol_table_offset = relocation_offset;
    for (index = 0; index < object->section_count; ++index) {
        symbol_table_offset += (uint32_t) (object->sections[index].relocation_count * 8u);
    }
    string_table_offset = symbol_table_offset + (uint32_t) (symbol_count * 16u);

    if (!chs_buffer_append_u32le(&file_buffer, CHS_MH_MAGIC_64, error) ||
        !chs_buffer_append_u32le(&file_buffer, cpu_type, error) ||
        !chs_buffer_append_u32le(&file_buffer, cpu_subtype, error) ||
        !chs_buffer_append_u32le(&file_buffer, CHS_MH_OBJECT, error) ||
        !chs_buffer_append_u32le(&file_buffer, object->has_build_version ? 4u : 3u, error) ||
        !chs_buffer_append_u32le(&file_buffer, sizeofcmds, error) ||
        !chs_buffer_append_u32le(&file_buffer, 0u, error) ||
        !chs_buffer_append_u32le(&file_buffer, 0u, error)) {
        goto cleanup;
    }

    if (!chs_buffer_append_u32le(&file_buffer, CHS_LC_SEGMENT_64, error) ||
        !chs_buffer_append_u32le(&file_buffer, load_command_size, error) ||
        !chs_macho_append_fixed_string(&file_buffer, "", 16u, error) ||
        !chs_buffer_append_u64le(&file_buffer, 0u, error) ||
        !chs_buffer_append_u64le(&file_buffer, total_section_size, error) ||
        !chs_buffer_append_u64le(&file_buffer, section_data_offset, error) ||
        !chs_buffer_append_u64le(&file_buffer, total_section_size, error) ||
        !chs_buffer_append_u32le(&file_buffer, 7u, error) ||
        !chs_buffer_append_u32le(&file_buffer, 7u, error) ||
        !chs_buffer_append_u32le(&file_buffer, (uint32_t) object->section_count, error) ||
        !chs_buffer_append_u32le(&file_buffer, 0u, error)) {
        goto cleanup;
    }

    {
        uint32_t running_data_offset;
        uint32_t running_relocation_offset;

        running_data_offset = section_data_offset;
        running_relocation_offset = relocation_offset;
        for (index = 0; index < object->section_count; ++index) {
            const ChsSection *section;

            section = &object->sections[index];
            if (!chs_macho_append_fixed_string(&file_buffer, section->name, 16u, error) ||
                !chs_macho_append_fixed_string(&file_buffer, section->segment_name, 16u, error) ||
                !chs_buffer_append_u64le(&file_buffer, 0u, error) ||
                !chs_buffer_append_u64le(&file_buffer, section->size, error) ||
                !chs_buffer_append_u32le(&file_buffer, running_data_offset, error) ||
                !chs_buffer_append_u32le(&file_buffer, chs_macho_log2_align(section->align), error) ||
                !chs_buffer_append_u32le(&file_buffer, running_relocation_offset, error) ||
                !chs_buffer_append_u32le(&file_buffer, (uint32_t) section->relocation_count, error) ||
                !chs_buffer_append_u32le(&file_buffer, section->macho_flags, error) ||
                !chs_buffer_append_u32le(&file_buffer, 0u, error) ||
                !chs_buffer_append_u32le(&file_buffer, 0u, error) ||
                !chs_buffer_append_u32le(&file_buffer, 0u, error)) {
                goto cleanup;
            }
            running_data_offset += (uint32_t) section->size;
            running_relocation_offset += (uint32_t) (section->relocation_count * 8u);
        }
    }

    if (!chs_buffer_append_u32le(&file_buffer, CHS_LC_SYMTAB, error) ||
        !chs_buffer_append_u32le(&file_buffer, 24u, error) ||
        !chs_buffer_append_u32le(&file_buffer, symbol_table_offset, error) ||
        !chs_buffer_append_u32le(&file_buffer, (uint32_t) symbol_count, error) ||
        !chs_buffer_append_u32le(&file_buffer, string_table_offset, error) ||
        !chs_buffer_append_u32le(&file_buffer, (uint32_t) string_table.length, error)) {
        goto cleanup;
    }

    if (!chs_buffer_append_u32le(&file_buffer, CHS_LC_DYSYMTAB, error) ||
        !chs_buffer_append_u32le(&file_buffer, 80u, error) ||
        !chs_buffer_append_u32le(&file_buffer, 0u, error) ||
        !chs_buffer_append_u32le(&file_buffer, (uint32_t) local_count, error) ||
        !chs_buffer_append_u32le(&file_buffer, (uint32_t) local_count, error) ||
        !chs_buffer_append_u32le(&file_buffer, (uint32_t) extdef_count, error) ||
        !chs_buffer_append_u32le(&file_buffer, (uint32_t) (local_count + extdef_count), error) ||
        !chs_buffer_append_u32le(&file_buffer, (uint32_t) undef_count, error)) {
        goto cleanup;
    }
    for (index = 0; index < 12; ++index) {
        if (!chs_buffer_append_u32le(&file_buffer, 0u, error)) {
            goto cleanup;
        }
    }

    if (object->has_build_version) {
        if (!chs_buffer_append_u32le(&file_buffer, CHS_LC_BUILD_VERSION, error) ||
            !chs_buffer_append_u32le(&file_buffer, 24u, error) ||
            !chs_buffer_append_u32le(&file_buffer, CHS_PLATFORM_MACOS, error) ||
            !chs_buffer_append_u32le(&file_buffer,
                                     chs_version_triplet(object->build_version_major,
                                                         object->build_version_minor,
                                                         object->build_version_patch),
                                     error) ||
            !chs_buffer_append_u32le(&file_buffer,
                                     chs_version_triplet(object->build_version_major,
                                                         object->build_version_minor,
                                                         object->build_version_patch),
                                     error) ||
            !chs_buffer_append_u32le(&file_buffer, 0u, error)) {
            goto cleanup;
        }
    }

    for (index = 0; index < object->section_count; ++index) {
        if (!chs_buffer_append(&file_buffer, object->sections[index].data, object->sections[index].size, error)) {
            goto cleanup;
        }
    }

    for (index = 0; index < object->section_count; ++index) {
        if (!chs_macho_append_relocations(&file_buffer, &object->sections[index], symbol_index_map, error)) {
            goto cleanup;
        }
    }

    if (!chs_macho_append_symbol_table(&file_buffer, symbols, symbol_count, error) ||
        !chs_buffer_append(&file_buffer, string_table.data, string_table.length, error)) {
        goto cleanup;
    }

    if (!chs_write_entire_file(output_path, file_buffer.data, file_buffer.length, error)) {
        goto cleanup;
    }

    success = true;

cleanup:
    chs_buffer_free(&file_buffer);
    chs_buffer_free(&string_table);
    free(sort_entries);
    free(symbols);
    free(symbol_index_map);
    return success;
}
