#include "chs/bslash.h"
#include "chs/bslash_embed.h"

#include "chs/common.h"
#include "chs/object.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#ifndef unlink
#define unlink _unlink
#endif
#else
#include <unistd.h>
#endif

#include "chs/bso_format.h"

static const char *chs_bslash_section_name(ChsOutputKind output_kind) {
    return output_kind == CHS_OUTPUT_MACHO ? "__text" : ".text";
}

static const char *chs_bslash_segment_name(ChsOutputKind output_kind) {
    return output_kind == CHS_OUTPUT_MACHO ? "__TEXT" : "";
}

static bool chs_bslash_run_bas(const char *input_path,
                               const char *output_path,
                               bool object_mode,
                               ChsError *error) {
    char bas_error[1024];

    bas_error[0] = '\0';
    if (bas_assemble_file(input_path,
                          output_path,
                          object_mode,
                          false,
                          0,
                          bas_error,
                          sizeof(bas_error)) != 0) {
        if (bas_error[0] != '\0') {
            chs_set_error(error, "%s", bas_error);
        } else {
            chs_set_error(error, "internal BAS assembly failed for %s", input_path);
        }
        return false;
    }
    return true;
}

static const char *chs_bslash_string_at(const uint8_t *string_table,
                                        size_t string_table_size,
                                        uint32_t offset,
                                        ChsError *error) {
    size_t index;

    if ((size_t) offset >= string_table_size) {
        chs_set_error(error, "BSO string offset %u is out of range", offset);
        return NULL;
    }

    for (index = offset; index < string_table_size; ++index) {
        if (string_table[index] == '\0') {
            return (const char *) string_table + offset;
        }
    }

    chs_set_error(error, "unterminated BSO string at offset %u", offset);
    return NULL;
}

static bool chs_bslash_write_addend(ChsSection *section,
                                    uint64_t offset,
                                    ChsRelocationKind kind,
                                    int64_t addend,
                                    ChsError *error) {
    size_t width;
    bool is_pc_relative;
    uint64_t encoded_value;
    size_t index;

    switch (kind) {
        case CHS_RELOC_BSLASH_ABS8:
            width = 1;
            is_pc_relative = false;
            break;
        case CHS_RELOC_BSLASH_ABS16:
            width = 2;
            is_pc_relative = false;
            break;
        case CHS_RELOC_BSLASH_ABS32:
            width = 4;
            is_pc_relative = false;
            break;
        case CHS_RELOC_BSLASH_ABS64:
            width = 8;
            is_pc_relative = false;
            break;
        case CHS_RELOC_BSLASH_REL8:
            width = 1;
            is_pc_relative = true;
            break;
        case CHS_RELOC_BSLASH_REL32:
            width = 4;
            is_pc_relative = true;
            break;
        default:
            chs_set_error(error, "unsupported BSlash relocation kind %d", (int) kind);
            return false;
    }

    if (offset + width > section->size) {
        chs_set_error(error, "BSO relocation at offset %llu overruns the translated section",
                      (unsigned long long) offset);
        return false;
    }

    if (is_pc_relative) {
        int64_t min_value;
        int64_t max_value;

        min_value = -(1ll << (width * 8u - 1u));
        max_value = (1ll << (width * 8u - 1u)) - 1ll;
        if (addend < min_value || addend > max_value) {
            chs_set_error(error, "BSlash PC-relative addend %lld does not fit in %zu bytes",
                          (long long) addend, width);
            return false;
        }
    } else {
        if (addend < 0) {
            chs_set_error(error, "negative absolute addend %lld is not supported for BSlash relocations",
                          (long long) addend);
            return false;
        }
        if (width < sizeof(uint64_t) && (uint64_t) addend >= (1ull << (width * 8u))) {
            chs_set_error(error, "BSlash absolute addend %lld does not fit in %zu bytes",
                          (long long) addend, width);
            return false;
        }
    }

    encoded_value = (uint64_t) addend;
    for (index = 0; index < width; ++index) {
        section->data[offset + index] = (uint8_t) ((encoded_value >> (index * 8u)) & 0xffu);
    }
    return true;
}

static bool chs_bslash_translate_bso(const char *bso_path,
                                     ChsOutputKind output_kind,
                                     ChsObject *object,
                                     ChsError *error) {
    char *file_data;
    size_t file_size;
    const bso_header_t *header;
    const uint8_t *cursor;
    const bso_symbol_record_t *symbol_records;
    const bso_relocation_record_t *relocation_records;
    const uint8_t *string_table;
    size_t section_index;
    size_t index;
    bool success;

    file_data = NULL;
    file_size = 0;
    section_index = 0;
    success = false;

    if (!chs_read_entire_file(bso_path, &file_data, &file_size, error)) {
        return false;
    }

    if (file_size < sizeof(*header)) {
        chs_set_error(error, "%s is too small to be a BSO object", bso_path);
        goto cleanup;
    }

    header = (const bso_header_t *) file_data;
    if (memcmp(header->magic, BSO_MAGIC, BSO_MAGIC_SIZE) != 0) {
        chs_set_error(error, "%s is not a BSO object", bso_path);
        goto cleanup;
    }
    if (header->version != BSO_VERSION) {
        chs_set_error(error, "%s has unsupported BSO version %u", bso_path, header->version);
        goto cleanup;
    }

    cursor = (const uint8_t *) file_data + sizeof(*header);
    if ((uint64_t) sizeof(*header) + header->code_size +
            (uint64_t) header->symbol_count * sizeof(*symbol_records) +
            (uint64_t) header->relocation_count * sizeof(*relocation_records) +
            header->string_table_size > file_size) {
        chs_set_error(error, "%s is truncated", bso_path);
        goto cleanup;
    }

    symbol_records = (const bso_symbol_record_t *) (cursor + header->code_size);
    relocation_records = (const bso_relocation_record_t *) (cursor + header->code_size +
                                                             (size_t) header->symbol_count * sizeof(*symbol_records));
    string_table = cursor + header->code_size +
                   (size_t) header->symbol_count * sizeof(*symbol_records) +
                   (size_t) header->relocation_count * sizeof(*relocation_records);

    object->arch = CHS_ARCH_BSLASH;
    object->output_kind = output_kind;
    if (!chs_object_get_or_create_section(object,
                                          chs_bslash_segment_name(output_kind),
                                          chs_bslash_section_name(output_kind),
                                          output_kind == CHS_OUTPUT_MACHO ? 0x80000000u : 0u,
                                          &section_index,
                                          error)) {
        goto cleanup;
    }
    if (!chs_section_append_data(&object->sections[section_index], cursor, header->code_size, error)) {
        goto cleanup;
    }

    for (index = 0; index < header->symbol_count; ++index) {
        const char *name;
        size_t symbol_index;
        ChsSymbol *symbol;

        name = chs_bslash_string_at(string_table, header->string_table_size,
                                    symbol_records[index].name_offset, error);
        if (name == NULL) {
            goto cleanup;
        }
        if (!chs_object_get_or_create_symbol(object, name, &symbol_index, error)) {
            goto cleanup;
        }
        symbol = &object->symbols[symbol_index];
        symbol->global_binding = (symbol_records[index].flags & BSO_SYMBOL_GLOBAL) != 0;
        symbol->defined = (symbol_records[index].flags & BSO_SYMBOL_DEFINED) != 0;
        symbol->external_reference = !symbol->defined;
        if (symbol->defined) {
            symbol->section_index = section_index;
            symbol->value = symbol_records[index].value;
        }
    }

    for (index = 0; index < header->relocation_count; ++index) {
        const bso_relocation_record_t *record;
        ChsRelocationKind kind;
        bool pc_relative;

        record = &relocation_records[index];
        if (record->symbol_index >= header->symbol_count) {
            chs_set_error(error, "BSO relocation %zu references symbol index %u outside the symbol table",
                          index, record->symbol_index);
            goto cleanup;
        }

        switch (record->kind) {
            case BSO_RELOC_ABS8:
                kind = CHS_RELOC_BSLASH_ABS8;
                pc_relative = false;
                break;
            case BSO_RELOC_ABS16:
                kind = CHS_RELOC_BSLASH_ABS16;
                pc_relative = false;
                break;
            case BSO_RELOC_ABS32:
                kind = CHS_RELOC_BSLASH_ABS32;
                pc_relative = false;
                break;
            case BSO_RELOC_ABS64:
                kind = CHS_RELOC_BSLASH_ABS64;
                pc_relative = false;
                break;
            case BSO_RELOC_REL8:
                kind = CHS_RELOC_BSLASH_REL8;
                pc_relative = true;
                break;
            case BSO_RELOC_REL32:
                kind = CHS_RELOC_BSLASH_REL32;
                pc_relative = true;
                break;
            default:
                chs_set_error(error, "unsupported BSO relocation kind %u", record->kind);
                goto cleanup;
        }

        if (!chs_bslash_write_addend(&object->sections[section_index], record->offset, kind, record->addend, error)) {
            goto cleanup;
        }
        if (!chs_section_add_relocation(&object->sections[section_index],
                                        record->offset,
                                        kind,
                                        record->symbol_index,
                                        record->addend,
                                        pc_relative,
                                        error)) {
            goto cleanup;
        }
    }

    success = true;

cleanup:
    free(file_data);
    return success;
}

bool chs_assemble_bslash_file(const ChsAssembleOptions *options, ChsError *error) {
    ChsString temp_path;
    ChsObject object;
    bool success;

    memset(&temp_path, 0, sizeof(temp_path));
    memset(&object, 0, sizeof(object));
    success = false;

    if (options->output_kind == CHS_OUTPUT_BIN) {
        success = chs_bslash_run_bas(options->input_path, options->output_path, false, error);
        goto cleanup;
    }

    if (options->output_kind != CHS_OUTPUT_ELF64 && options->output_kind != CHS_OUTPUT_MACHO) {
        chs_set_error(error, "unsupported BSlash output format");
        goto cleanup;
    }

    if (!chs_make_temp_path(".bso", &temp_path, error)) {
        goto cleanup;
    }
    if (!chs_bslash_run_bas(options->input_path, temp_path.data, true, error)) {
        goto cleanup;
    }
    if (!chs_bslash_translate_bso(temp_path.data, options->output_kind, &object, error)) {
        goto cleanup;
    }

    if (options->output_kind == CHS_OUTPUT_MACHO) {
        success = chs_write_macho_object(&object, options->output_path, error);
    } else {
        success = chs_write_elf64_object(&object, options->output_path, error);
    }

cleanup:
    if (temp_path.data != NULL) {
        unlink(temp_path.data);
    }
    chs_object_free(&object);
    chs_string_free(&temp_path);
    return success;
}
