#include "chs/arch.h"
#include "chs/assembler.h"
#include "chs/bslash.h"
#include "chs/object.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t section_index;
    uint64_t offset;
    char *mnemonic;
    char *operands;
} ChsPendingInstruction;

typedef struct {
    ChsPendingInstruction *data;
    size_t count;
    size_t capacity;
} ChsPendingInstructionList;

static bool chs_pending_instruction_list_append(ChsPendingInstructionList *list,
                                                size_t section_index,
                                                uint64_t offset,
                                                const char *mnemonic,
                                                const char *operands,
                                                ChsError *error) {
    size_t new_capacity;
    ChsPendingInstruction *new_data;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0 ? 32 : list->capacity * 2;
        new_data = realloc(list->data, new_capacity * sizeof(*new_data));
        if (new_data == NULL) {
            chs_set_error(error, "out of memory growing instruction list");
            return false;
        }
        list->data = new_data;
        list->capacity = new_capacity;
    }

    list->data[list->count].section_index = section_index;
    list->data[list->count].offset = offset;
    list->data[list->count].mnemonic = chs_strdup(mnemonic, error);
    if (list->data[list->count].mnemonic == NULL) {
        return false;
    }
    list->data[list->count].operands = chs_strdup(operands, error);
    if (list->data[list->count].operands == NULL) {
        return false;
    }
    ++list->count;
    return true;
}


static void chs_pending_instruction_list_free(ChsPendingInstructionList *list) {
    size_t index;

    for (index = 0; index < list->count; ++index) {
        free(list->data[index].mnemonic);
        free(list->data[index].operands);
    }
    free(list->data);
    memset(list, 0, sizeof(*list));
}

static uint32_t chs_parse_macho_section_flags(const char *section_name, char **attributes, size_t attribute_count) {
    size_t index;
    uint32_t flags;

    flags = 0;
    if (strcmp(section_name, "__cstring") == 0) {
        flags = 0x00000002u;
    }
    for (index = 0; index < attribute_count; ++index) {
        if (strcmp(attributes[index], "pure_instructions") == 0) {
            flags |= 0x80000000u;
        } else if (strcmp(attributes[index], "some_instructions") == 0) {
            flags |= 0x00000400u;
        }
    }
    return flags;
}

static const char *chs_map_elf_section_name(const char *segment_name, const char *section_name) {
    if (strcmp(segment_name, "__TEXT") == 0 && strcmp(section_name, "__text") == 0) {
        return ".text";
    }
    if (strcmp(segment_name, "__TEXT") == 0 && strcmp(section_name, "__cstring") == 0) {
        return ".rodata.str1.1";
    }
    if (strcmp(segment_name, "__DATA") == 0 && strcmp(section_name, "__const") == 0) {
        return ".rodata";
    }
    return section_name;
}

static size_t chs_split_csv(char *text, char **parts, size_t capacity) {
    size_t count;
    char *cursor;
    char *start;
    unsigned in_quote;

    count = 0;
    cursor = text;
    start = text;
    in_quote = 0;
    while (*cursor != '\0') {
        if (*cursor == '"') {
            in_quote ^= 1u;
        } else if (*cursor == ',' && !in_quote) {
            *cursor = '\0';
            if (count < capacity) {
                parts[count++] = chs_trim(start);
            }
            start = cursor + 1;
        }
        ++cursor;
    }
    if (count < capacity && *start != '\0') {
        parts[count++] = chs_trim(start);
    }
    return count;
}

static bool chs_parse_byte_directive(ChsSection *section, char *arguments, ChsError *error) {
    char *parts[256];
    size_t count;
    size_t index;

    count = chs_split_csv(arguments, parts, 256);
    for (index = 0; index < count; ++index) {
        uint64_t value;

        if (!chs_parse_u64(parts[index], &value) || value > 0xffu) {
            chs_set_error(error, "invalid .byte value: %s", parts[index]);
            return false;
        }
        if (!chs_section_append_data(section, &(uint8_t) { (uint8_t) value }, 1, error)) {
            return false;
        }
    }
    return true;
}

static bool chs_append_integer_bytes(ChsSection *section,
                                     uint64_t value,
                                     size_t width,
                                     ChsError *error) {
    uint8_t bytes[8];
    size_t index;

    if (width == 0 || width > sizeof(bytes)) {
        chs_set_error(error, "invalid integer width %zu", width);
        return false;
    }
    for (index = 0; index < width; ++index) {
        bytes[index] = (uint8_t) ((value >> (index * 8u)) & 0xffu);
    }
    return chs_section_append_data(section, bytes, width, error);
}

static bool chs_parse_integer_directive(ChsSection *section,
                                        char *arguments,
                                        size_t width,
                                        const char *directive,
                                        ChsError *error) {
    char *parts[256];
    size_t count;
    size_t index;

    count = chs_split_csv(arguments, parts, 256);
    for (index = 0; index < count; ++index) {
        uint64_t uvalue;
        int64_t svalue;
        char *token;

        token = chs_trim(parts[index]);
        if (*token == '\0') {
            continue;
        }
        if (chs_parse_i64(token, &svalue)) {
            uvalue = (uint64_t) svalue;
        } else if (chs_parse_u64(token, &uvalue)) {
            /* keep parsed unsigned value */
        } else {
            chs_set_error(error, "invalid %s value: %s", directive, token);
            return false;
        }
        if (!chs_append_integer_bytes(section, uvalue, width, error)) {
            return false;
        }
    }
    return true;
}

static bool chs_append_fill_bytes(ChsSection *section,
                                  size_t count,
                                  uint8_t fill,
                                  ChsError *error) {
    uint8_t chunk[256];

    if (count == 0) {
        return true;
    }
    if (fill == 0) {
        return chs_section_append_zeros(section, count, error);
    }
    memset(chunk, fill, sizeof(chunk));
    while (count > 0) {
        size_t to_write;
        to_write = count < sizeof(chunk) ? count : sizeof(chunk);
        if (!chs_section_append_data(section, chunk, to_write, error)) {
            return false;
        }
        count -= to_write;
    }
    return true;
}

static bool chs_parse_fill_directive(ChsSection *section,
                                     char *arguments,
                                     const char *directive,
                                     ChsError *error) {
    char *parts[3];
    size_t count;
    uint64_t amount;
    uint64_t fill_value;

    count = chs_split_csv(arguments, parts, 3);
    if (count == 0 || !chs_parse_u64(chs_trim(parts[0]), &amount)) {
        chs_set_error(error, "invalid %s value: %s", directive, arguments);
        return false;
    }
    fill_value = 0;
    if (count >= 2) {
        if (!chs_parse_u64(chs_trim(parts[1]), &fill_value) || fill_value > 0xffu) {
            chs_set_error(error, "invalid %s fill value: %s", directive, parts[1]);
            return false;
        }
    }
    return chs_append_fill_bytes(section, (size_t) amount, (uint8_t) fill_value, error);
}

static bool chs_append_string_literal(ChsSection *section,
                                      const char *token,
                                      bool append_nul,
                                      ChsError *error) {
    size_t length;
    size_t index;
    uint8_t out;

    length = strlen(token);
    if (length < 2 || token[0] != '"' || token[length - 1] != '"') {
        uint64_t value;
        if (!chs_parse_u64(token, &value) || value > 0xffu) {
            chs_set_error(error, "invalid string token: %s", token);
            return false;
        }
        if (!chs_append_integer_bytes(section, value, 1, error)) {
            return false;
        }
        return true;
    }

    for (index = 1; index + 1 < length; ++index) {
        uint8_t out;
        char c;

        c = token[index];
        if (c != '\\') {
            out = (uint8_t) c;
            if (!chs_section_append_data(section, &out, 1, error)) {
                return false;
            }
            continue;
        }

        ++index;
        if (index + 1 > length) {
            break;
        }
        c = token[index];
        switch (c) {
            case 'n':
                out = (uint8_t) '\n';
                break;
            case 'r':
                out = (uint8_t) '\r';
                break;
            case 't':
                out = (uint8_t) '\t';
                break;
            case '0':
                out = 0;
                break;
            case '\\':
                out = (uint8_t) '\\';
                break;
            case '"':
                out = (uint8_t) '"';
                break;
            case 'x': {
                char hexbuf[5] = {'0', 'x', '\0', '\0', '\0'};
                uint64_t value;
                if (index + 2 >= length) {
                    chs_set_error(error, "invalid hex escape in string: %s", token);
                    return false;
                }
                hexbuf[2] = token[index + 1];
                hexbuf[3] = token[index + 2];
                if (!chs_parse_u64(hexbuf, &value) || value > 0xffu) {
                    chs_set_error(error, "invalid hex escape in string: %s", token);
                    return false;
                }
                out = (uint8_t) value;
                index += 2;
                break;
            }
            default:
                out = (uint8_t) c;
                break;
        }
        if (!chs_section_append_data(section, &out, 1, error)) {
            return false;
        }
    }

    if (append_nul) {
        out = 0;
        if (!chs_section_append_data(section, &out, 1, error)) {
            return false;
        }
    }
    return true;
}

static bool chs_parse_string_directive(ChsSection *section,
                                       char *arguments,
                                       bool append_nul,
                                       ChsError *error) {
    char *parts[256];
    size_t count;
    size_t index;

    count = chs_split_csv(arguments, parts, 256);
    for (index = 0; index < count; ++index) {
        char *token;
        token = chs_trim(parts[index]);
        if (*token == '\0') {
            continue;
        }
        if (!chs_append_string_literal(section, token, append_nul, error)) {
            return false;
        }
    }
    return true;
}

static bool chs_handle_directive(ChsObject *object,
                                 size_t *current_section_index,
                                 const char *directive,
                                 char *arguments,
                                 ChsError *error) {
    if (strcmp(directive, ".extern") == 0) {
        size_t symbol_index;

        if (!chs_object_get_or_create_symbol(object, chs_trim(arguments), &symbol_index, error)) {
            return false;
        }
        object->symbols[symbol_index].external_reference = true;
        object->symbols[symbol_index].global_binding = true;
        return true;
    }

    if (strcmp(directive, ".globl") == 0) {
        size_t symbol_index;

        if (!chs_object_get_or_create_symbol(object, chs_trim(arguments), &symbol_index, error)) {
            return false;
        }
        object->symbols[symbol_index].global_binding = true;
        return true;
    }

    if (strcmp(directive, ".global") == 0) {
        return chs_handle_directive(object, current_section_index, ".globl", arguments, error);
    }

    if (strcmp(directive, ".text") == 0) {
        if (object->output_kind == CHS_OUTPUT_MACHO) {
            return chs_object_get_or_create_section(object, "__TEXT", "__text", 0x80000000u,
                                                    current_section_index, error);
        }
        return chs_object_get_or_create_section(object, "", ".text", 0,
                                                current_section_index, error);
    }

    if (strcmp(directive, ".data") == 0) {
        if (object->output_kind == CHS_OUTPUT_MACHO) {
            return chs_object_get_or_create_section(object, "__DATA", "__data", 0,
                                                    current_section_index, error);
        }
        return chs_object_get_or_create_section(object, "", ".data", 0,
                                                current_section_index, error);
    }

    if (strcmp(directive, ".bss") == 0) {
        if (object->output_kind == CHS_OUTPUT_MACHO) {
            return chs_object_get_or_create_section(object, "__DATA", "__bss", 0,
                                                    current_section_index, error);
        }
        return chs_object_get_or_create_section(object, "", ".bss", 0,
                                                current_section_index, error);
    }

    if (strcmp(directive, ".section") == 0) {
        char *parts[8];
        size_t count;
        char *segment_name;
        char *section_name;
        uint32_t flags;

        count = chs_split_csv(arguments, parts, 8);
        if (count == 0) {
            chs_set_error(error, "missing .section arguments");
            return false;
        }
        if (object->output_kind == CHS_OUTPUT_MACHO) {
            if (count < 2) {
                chs_set_error(error, "Mach-O .section requires segment and section names");
                return false;
            }
            segment_name = parts[0];
            section_name = parts[1];
            flags = chs_parse_macho_section_flags(section_name, parts + 2, count - 2);
        } else {
            if (count >= 2 && parts[0][0] == '_' && parts[1][0] == '_') {
                segment_name = "";
                section_name = (char *) chs_map_elf_section_name(parts[0], parts[1]);
            } else {
                segment_name = "";
                section_name = parts[0];
            }
            flags = 0;
        }
        return chs_object_get_or_create_section(object, segment_name, section_name, flags, current_section_index, error);
    }

    if (strcmp(directive, ".p2align") == 0) {
        uint64_t power;
        uint64_t alignment;

        if (*current_section_index == (size_t) -1) {
            chs_set_error(error, ".p2align without an active section");
            return false;
        }
        if (!chs_parse_u64(chs_trim(arguments), &power) || power > 31) {
            chs_set_error(error, "invalid .p2align value: %s", arguments);
            return false;
        }
        alignment = 1ull << power;
        return chs_section_align(&object->sections[*current_section_index], alignment, error);
    }

    if (strcmp(directive, ".byte") == 0) {
        if (*current_section_index == (size_t) -1) {
            chs_set_error(error, ".byte without an active section");
            return false;
        }
        return chs_parse_byte_directive(&object->sections[*current_section_index], arguments, error);
    }

    if (strcmp(directive, ".short") == 0 || strcmp(directive, ".hword") == 0 ||
        strcmp(directive, ".2byte") == 0) {
        if (*current_section_index == (size_t) -1) {
            chs_set_error(error, "%s without an active section", directive);
            return false;
        }
        return chs_parse_integer_directive(&object->sections[*current_section_index],
                                           arguments, 2, directive, error);
    }

    if (strcmp(directive, ".long") == 0 || strcmp(directive, ".word") == 0 ||
        strcmp(directive, ".4byte") == 0) {
        if (*current_section_index == (size_t) -1) {
            chs_set_error(error, "%s without an active section", directive);
            return false;
        }
        return chs_parse_integer_directive(&object->sections[*current_section_index],
                                           arguments, 4, directive, error);
    }

    if (strcmp(directive, ".quad") == 0 || strcmp(directive, ".8byte") == 0) {
        if (*current_section_index == (size_t) -1) {
            chs_set_error(error, "%s without an active section", directive);
            return false;
        }
        return chs_parse_integer_directive(&object->sections[*current_section_index],
                                           arguments, 8, directive, error);
    }

    if (strcmp(directive, ".zero") == 0 || strcmp(directive, ".space") == 0) {
        if (*current_section_index == (size_t) -1) {
            chs_set_error(error, "%s without an active section", directive);
            return false;
        }
        return chs_parse_fill_directive(&object->sections[*current_section_index],
                                        arguments, directive, error);
    }

    if (strcmp(directive, ".ascii") == 0 || strcmp(directive, ".asciz") == 0 ||
        strcmp(directive, ".string") == 0) {
        bool append_nul;
        if (*current_section_index == (size_t) -1) {
            chs_set_error(error, "%s without an active section", directive);
            return false;
        }
        append_nul = (strcmp(directive, ".ascii") != 0);
        return chs_parse_string_directive(&object->sections[*current_section_index],
                                          arguments, append_nul, error);
    }

    if (strcmp(directive, ".build_version") == 0) {
        char *parts[4];
        size_t count;
        uint64_t major;
        uint64_t minor;

        count = chs_split_csv(arguments, parts, 4);
        if (count < 3) {
            chs_set_error(error, "invalid .build_version directive");
            return false;
        }
        if (strcmp(chs_trim(parts[0]), "macos") != 0) {
            chs_set_error(error, "unsupported .build_version platform: %s", parts[0]);
            return false;
        }
        if (!chs_parse_u64(parts[1], &major) || !chs_parse_u64(parts[2], &minor)) {
            chs_set_error(error, "invalid .build_version numeric arguments");
            return false;
        }
        object->build_version_major = (unsigned) major;
        object->build_version_minor = (unsigned) minor;
        object->build_version_patch = 0;
        object->has_build_version = true;
        return true;
    }

    if (strcmp(directive, ".file") == 0 || strcmp(directive, ".loc") == 0) {
        return true;
    }

    if (strncmp(directive, ".cfi_", 5) == 0 ||
        strcmp(directive, ".type") == 0 ||
        strcmp(directive, ".size") == 0 ||
        strcmp(directive, ".ident") == 0 ||
        strcmp(directive, ".subsections_via_symbols") == 0) {
        return true;
    }

    chs_set_error(error, "unsupported directive: %s", directive);
    return false;
}

static void chs_patch_u32le(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t) (value & 0xffu);
    data[1] = (uint8_t) ((value >> 8) & 0xffu);
    data[2] = (uint8_t) ((value >> 16) & 0xffu);
    data[3] = (uint8_t) ((value >> 24) & 0xffu);
}

static bool chs_emit_output(const ChsObject *object, const char *output_path, ChsOutputKind output_kind, ChsError *error) {
    if (output_kind == CHS_OUTPUT_MACHO) {
        return chs_write_macho_object(object, output_path, error);
    }
    if (output_kind == CHS_OUTPUT_ELF64) {
        return chs_write_elf64_object(object, output_path, error);
    }
    chs_set_error(error, "output format is not supported by the generic assembler path");
    return false;
}

bool chs_assemble_file(const ChsAssembleOptions *options, ChsError *error) {
    const ChsArchOps *arch_ops;
    ChsObject object;
    ChsPendingInstructionList instructions;
    char *source;
    size_t source_size;
    char *cursor;
    size_t current_section_index;
    bool success;
    size_t instruction_index;

    memset(&object, 0, sizeof(object));
    memset(&instructions, 0, sizeof(instructions));
    source = NULL;
    source_size = 0;
    cursor = NULL;
    current_section_index = (size_t) -1;
    success = false;

    if (options->arch == CHS_ARCH_BSLASH) {
        return chs_assemble_bslash_file(options, error);
    }

    object.arch = options->arch;
    object.output_kind = options->output_kind;
    if (options->output_kind == CHS_OUTPUT_BIN) {
        chs_set_error(error, "--format bin is only supported for --arch bslash");
        return false;
    }
    arch_ops = chs_find_arch_ops(options->arch);
    if (arch_ops == NULL) {
        chs_set_error(error, "unsupported architecture kind");
        return false;
    }

    if (!chs_read_entire_file(options->input_path, &source, &source_size, error)) {
        return false;
    }

    cursor = source;
    while (*cursor != '\0') {
        char *line_start;
        char *line_end;
        char *line;

        line_start = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            ++cursor;
        }
        line_end = cursor;
        if (*cursor == '\n') {
            *cursor++ = '\0';
        }
        (void) line_end;
        chs_strip_line_comment(line_start);
        line = chs_trim(line_start);
        if (*line == '\0') {
            continue;
        }

        while (1) {
            char *colon;

            colon = strchr(line, ':');
            if (colon == NULL) {
                break;
            }
            if (colon[1] != '\0' && !isspace((unsigned char) colon[1])) {
                break;
            }
            *colon = '\0';
            if (*line != '\0') {
                size_t symbol_index;

                if (current_section_index == (size_t) -1) {
                    chs_set_error(error, "label outside of a section: %s", line);
                    goto cleanup;
                }
                if (!chs_object_get_or_create_symbol(&object, chs_trim(line), &symbol_index, error)) {
                    goto cleanup;
                }
                object.symbols[symbol_index].defined = true;
                object.symbols[symbol_index].section_index = current_section_index;
                object.symbols[symbol_index].value = object.sections[current_section_index].size;
            }
            line = chs_trim(colon + 1);
            if (*line == '\0') {
                break;
            }
        }
        if (*line == '\0') {
            continue;
        }

        if (line[0] == '.') {
            char *space;
            char *directive;
            char *arguments;

            space = line;
            while (*space != '\0' && !isspace((unsigned char) *space)) {
                ++space;
            }
            directive = line;
            arguments = space;
            if (*space != '\0') {
                *space++ = '\0';
                arguments = chs_trim(space);
            } else {
                arguments = space;
            }
            if (!chs_handle_directive(&object, &current_section_index, directive, arguments, error)) {
                goto cleanup;
            }
            continue;
        }

        {
            char *space;
            char *mnemonic;
            char *operands;
            uint8_t placeholder[4] = {0, 0, 0, 0};

            if (current_section_index == (size_t) -1) {
                chs_set_error(error, "instruction outside of a section: %s", line);
                goto cleanup;
            }

            space = line;
            while (*space != '\0' && !isspace((unsigned char) *space)) {
                ++space;
            }
            mnemonic = line;
            operands = space;
            if (*space != '\0') {
                *space++ = '\0';
                operands = chs_trim(space);
            } else {
                operands = "";
            }
            if (!chs_pending_instruction_list_append(&instructions,
                                                    current_section_index,
                                                    object.sections[current_section_index].size,
                                                    mnemonic,
                                                    operands,
                                                    error)) {
                goto cleanup;
            }
            if (!chs_section_append_data(&object.sections[current_section_index], placeholder, sizeof(placeholder), error)) {
                goto cleanup;
            }
        }
    }

    for (instruction_index = 0; instruction_index < instructions.count; ++instruction_index) {
        ChsPendingInstruction *instruction;
        ChsSection *section;
        ChsEncodedInstruction encoded;

        instruction = &instructions.data[instruction_index];
        section = &object.sections[instruction->section_index];
        if (!arch_ops->encode_instruction(&object,
                                          section,
                                          instruction->offset,
                                          instruction->mnemonic,
                                          instruction->operands,
                                          &encoded,
                                          error)) {
            goto cleanup;
        }
        chs_patch_u32le(section->data + instruction->offset, encoded.encoded);
        if (encoded.uses_symbol) {
            size_t symbol_index;
            if (!chs_object_get_or_create_symbol(&object, encoded.symbol_name, &symbol_index, error)) {
                free((void *) encoded.symbol_name);
                goto cleanup;
            }
            if (!chs_section_add_relocation(section,
                                           instruction->offset,
                                           encoded.relocation_kind,
                                           symbol_index,
                                           0,
                                           encoded.pc_relative,
                                           error)) {
                free((void *) encoded.symbol_name);
                goto cleanup;
            }
            free((void *) encoded.symbol_name);
        }
    }

    if (!chs_emit_output(&object, options->output_path, options->output_kind, error)) {
        goto cleanup;
    }

    success = true;

cleanup:
    free(source);
    chs_pending_instruction_list_free(&instructions);
    chs_object_free(&object);
    return success;
}
