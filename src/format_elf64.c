#include "chs/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHS_ET_REL 1u
#define CHS_EM_X86_64 62u
#define CHS_EM_AARCH64 183u
#define CHS_SHT_NULL 0u
#define CHS_SHT_PROGBITS 1u
#define CHS_SHT_SYMTAB 2u
#define CHS_SHT_STRTAB 3u
#define CHS_SHT_RELA 4u
#define CHS_SHF_WRITE 0x1u
#define CHS_SHF_ALLOC 0x2u
#define CHS_SHF_EXECINSTR 0x4u

#define CHS_STB_LOCAL 0u
#define CHS_STB_GLOBAL 1u
#define CHS_STT_NOTYPE 0u

#define CHS_R_AARCH64_ADR_PREL_PG_HI21 275u
#define CHS_R_AARCH64_ADD_ABS_LO12_NC 277u
#define CHS_R_AARCH64_CALL26 283u

typedef struct {
    uint32_t name_index;
    uint32_t type;
    uint64_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t alignment;
    uint64_t entry_size;
} ChsElfSectionHeader;

typedef struct {
    uint32_t name;
    unsigned char info;
    unsigned char other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} ChsElfSymbol;

static unsigned chs_elf_machine(const ChsObject *object) {
    return object->arch == CHS_ARCH_X86_64 ? CHS_EM_X86_64 : CHS_EM_AARCH64;
}


static uint32_t chs_elf_section_type(const ChsSection *section) {
    (void) section;
    return CHS_SHT_PROGBITS;
}

static uint64_t chs_elf_section_flags(const ChsSection *section) {
    if (strcmp(section->name, ".text") == 0 || strcmp(section->name, "__text") == 0) {
        return CHS_SHF_ALLOC | CHS_SHF_EXECINSTR;
    }
    if (strncmp(section->name, ".rodata", 7) == 0 || strcmp(section->name, "__const") == 0 || strcmp(section->name, "__cstring") == 0) {
        return CHS_SHF_ALLOC;
    }
    return CHS_SHF_ALLOC | CHS_SHF_WRITE;
}

static const char *chs_elf_relocation_section_name(const ChsSection *section, char *buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, ".rela%s", section->name[0] == '.' ? section->name : ".text");
    return buffer;
}

static uint32_t chs_elf_relocation_type(const ChsRelocation *relocation) {
    switch (relocation->kind) {
        case CHS_RELOC_AARCH64_BRANCH26:
            return CHS_R_AARCH64_CALL26;
        case CHS_RELOC_AARCH64_PAGE21:
            return CHS_R_AARCH64_ADR_PREL_PG_HI21;
        case CHS_RELOC_AARCH64_PAGEOFF12:
            return CHS_R_AARCH64_ADD_ABS_LO12_NC;
    }
    return 0;
}

static int chs_elf_symbol_rank(const ChsSymbol *symbol) {
    return symbol->global_binding || symbol->external_reference || !symbol->defined ? 1 : 0;
}

bool chs_write_elf64_object(const ChsObject *object, const char *output_path, ChsError *error) {
    ChsBuffer buffer;
    ChsBuffer shstrtab;
    ChsBuffer strtab;
    ChsElfSectionHeader *section_headers;
    ChsElfSymbol *symbols;
    size_t *symbol_order;
    uint32_t *symbol_index_map;
    size_t relocation_section_count;
    size_t total_section_headers;
    size_t symbol_count;
    size_t local_symbol_count;
    size_t section_index;
    size_t symbol_index;
    uint64_t section_header_offset;
    bool success;

    memset(&buffer, 0, sizeof(buffer));
    memset(&shstrtab, 0, sizeof(shstrtab));
    memset(&strtab, 0, sizeof(strtab));
    section_headers = NULL;
    symbols = NULL;
    symbol_order = NULL;
    symbol_index_map = NULL;
    success = false;

    relocation_section_count = 0;
    for (section_index = 0; section_index < object->section_count; ++section_index) {
        if (object->sections[section_index].relocation_count != 0) {
            ++relocation_section_count;
        }
    }

    symbol_count = object->symbol_count + 1;
    total_section_headers = 1 + object->section_count + relocation_section_count + 3;
    section_headers = calloc(total_section_headers, sizeof(*section_headers));
    symbols = calloc(symbol_count, sizeof(*symbols));
    symbol_order = calloc(object->symbol_count == 0 ? 1 : object->symbol_count, sizeof(*symbol_order));
    symbol_index_map = calloc(object->symbol_count == 0 ? 1 : object->symbol_count, sizeof(*symbol_index_map));
    if (section_headers == NULL || symbols == NULL || symbol_order == NULL || symbol_index_map == NULL) {
        chs_set_error(error, "out of memory allocating ELF tables");
        goto cleanup;
    }

    if (!chs_buffer_append_u8(&shstrtab, 0, error) || !chs_buffer_append_u8(&strtab, 0, error)) {
        goto cleanup;
    }

    for (symbol_index = 0; symbol_index < object->symbol_count; ++symbol_index) {
        symbol_order[symbol_index] = symbol_index;
    }
    for (symbol_index = 1; symbol_index < object->symbol_count; ++symbol_index) {
        size_t rank_index;

        rank_index = symbol_index;
        while (rank_index > 0 &&
               chs_elf_symbol_rank(&object->symbols[symbol_order[rank_index - 1]]) >
                   chs_elf_symbol_rank(&object->symbols[symbol_order[rank_index]])) {
            size_t temporary;

            temporary = symbol_order[rank_index - 1];
            symbol_order[rank_index - 1] = symbol_order[rank_index];
            symbol_order[rank_index] = temporary;
            --rank_index;
        }
    }

    local_symbol_count = 1;
    for (symbol_index = 0; symbol_index < object->symbol_count; ++symbol_index) {
        const ChsSymbol *symbol;
        uint32_t name_index;
        unsigned char info;
        uint16_t shndx;
        uint64_t value;

        symbol = &object->symbols[symbol_order[symbol_index]];
        symbol_index_map[symbol_order[symbol_index]] = (uint32_t) (symbol_index + 1);
        name_index = (uint32_t) strtab.length;
        if (!chs_buffer_append(&strtab, symbol->name, strlen(symbol->name) + 1, error)) {
            goto cleanup;
        }
        info = (unsigned char) ((chs_elf_symbol_rank(symbol) == 0 ? CHS_STB_LOCAL : CHS_STB_GLOBAL) << 4);
        shndx = symbol->defined ? (uint16_t) (symbol->section_index + 1) : 0u;
        value = symbol->defined ? symbol->value : 0u;
        if (chs_elf_symbol_rank(symbol) == 0) {
            ++local_symbol_count;
        }
        symbols[symbol_index + 1].name = name_index;
        symbols[symbol_index + 1].info = info | CHS_STT_NOTYPE;
        symbols[symbol_index + 1].other = 0;
        symbols[symbol_index + 1].shndx = shndx;
        symbols[symbol_index + 1].value = value;
        symbols[symbol_index + 1].size = 0;
    }

    if (!chs_buffer_append(&buffer, "\177ELF", 4, error) ||
        !chs_buffer_append_u8(&buffer, 2u, error) ||
        !chs_buffer_append_u8(&buffer, 1u, error) ||
        !chs_buffer_append_u8(&buffer, 1u, error) ||
        !chs_buffer_append_u8(&buffer, 0u, error)) {
        goto cleanup;
    }
    while (buffer.length < 16u) {
        if (!chs_buffer_append_u8(&buffer, 0u, error)) {
            goto cleanup;
        }
    }
    if (!chs_buffer_append_u16le(&buffer, CHS_ET_REL, error) ||
        !chs_buffer_append_u16le(&buffer, (uint16_t) chs_elf_machine(object), error) ||
        !chs_buffer_append_u32le(&buffer, 1u, error) ||
        !chs_buffer_append_u64le(&buffer, 0u, error) ||
        !chs_buffer_append_u64le(&buffer, 0u, error) ||
        !chs_buffer_append_u64le(&buffer, 0u, error) ||
        !chs_buffer_append_u32le(&buffer, 0u, error) ||
        !chs_buffer_append_u16le(&buffer, 64u, error) ||
        !chs_buffer_append_u16le(&buffer, 0u, error) ||
        !chs_buffer_append_u16le(&buffer, 0u, error) ||
        !chs_buffer_append_u16le(&buffer, 64u, error) ||
        !chs_buffer_append_u16le(&buffer, (uint16_t) total_section_headers, error) ||
        !chs_buffer_append_u16le(&buffer, (uint16_t) (total_section_headers - 1), error)) {
        goto cleanup;
    }

    for (section_index = 0; section_index < object->section_count; ++section_index) {
        uint64_t alignment;

        section_headers[1 + section_index].name_index = (uint32_t) shstrtab.length;
        if (!chs_buffer_append(&shstrtab, object->sections[section_index].name, strlen(object->sections[section_index].name) + 1, error)) {
            goto cleanup;
        }
        section_headers[1 + section_index].type = chs_elf_section_type(&object->sections[section_index]);
        section_headers[1 + section_index].flags = chs_elf_section_flags(&object->sections[section_index]);
        section_headers[1 + section_index].alignment = object->sections[section_index].align;
        section_headers[1 + section_index].entry_size = 0;
        alignment = object->sections[section_index].align == 0 ? 1u : object->sections[section_index].align;
        while ((buffer.length % alignment) != 0) {
            if (!chs_buffer_append_u8(&buffer, 0u, error)) {
                goto cleanup;
            }
        }
        section_headers[1 + section_index].offset = buffer.length;
        section_headers[1 + section_index].size = object->sections[section_index].size;
        if (!chs_buffer_append(&buffer, object->sections[section_index].data, object->sections[section_index].size, error)) {
            goto cleanup;
        }
    }

    {
        size_t relocation_header_index;

        relocation_header_index = 1 + object->section_count;
        for (section_index = 0; section_index < object->section_count; ++section_index) {
            if (object->sections[section_index].relocation_count == 0) {
                continue;
            }
            {
                char name_buffer[64];
                size_t relocation_index;

                section_headers[relocation_header_index].name_index = (uint32_t) shstrtab.length;
                if (!chs_buffer_append(&shstrtab,
                                       chs_elf_relocation_section_name(&object->sections[section_index], name_buffer, sizeof(name_buffer)),
                                       strlen(name_buffer) + 1,
                                       error)) {
                    goto cleanup;
                }
                section_headers[relocation_header_index].type = CHS_SHT_RELA;
                section_headers[relocation_header_index].flags = 0;
                section_headers[relocation_header_index].link = (uint32_t) (total_section_headers - 2);
                section_headers[relocation_header_index].info = (uint32_t) (1 + section_index);
                section_headers[relocation_header_index].alignment = 8u;
                section_headers[relocation_header_index].entry_size = 24u;
                buffer.length = chs_align_up_u64(buffer.length, 8u);
                while ((buffer.length & 7u) != 0) {
                    if (!chs_buffer_append_u8(&buffer, 0u, error)) {
                        goto cleanup;
                    }
                }
                section_headers[relocation_header_index].offset = buffer.length;
                section_headers[relocation_header_index].size = object->sections[section_index].relocation_count * 24u;
                for (relocation_index = 0; relocation_index < object->sections[section_index].relocation_count; ++relocation_index) {
                    uint64_t info;
                    info = ((uint64_t) symbol_index_map[object->sections[section_index].relocations[relocation_index].symbol_index] << 32) |
                           chs_elf_relocation_type(&object->sections[section_index].relocations[relocation_index]);
                    if (!chs_buffer_append_u64le(&buffer, object->sections[section_index].relocations[relocation_index].offset, error) ||
                        !chs_buffer_append_u64le(&buffer, info, error) ||
                        !chs_buffer_append_u64le(&buffer, (uint64_t) object->sections[section_index].relocations[relocation_index].addend, error)) {
                        goto cleanup;
                    }
                }
                ++relocation_header_index;
            }
        }
    }

    section_headers[total_section_headers - 3].name_index = (uint32_t) shstrtab.length;
    if (!chs_buffer_append(&shstrtab, ".symtab", 8, error)) {
        goto cleanup;
    }
    section_headers[total_section_headers - 3].type = CHS_SHT_SYMTAB;
    section_headers[total_section_headers - 3].link = (uint32_t) (total_section_headers - 2);
    section_headers[total_section_headers - 3].info = (uint32_t) local_symbol_count;
    section_headers[total_section_headers - 3].alignment = 8u;
    section_headers[total_section_headers - 3].entry_size = 24u;
    while ((buffer.length & 7u) != 0) {
        if (!chs_buffer_append_u8(&buffer, 0u, error)) {
            goto cleanup;
        }
    }
    section_headers[total_section_headers - 3].offset = buffer.length;
    section_headers[total_section_headers - 3].size = symbol_count * 24u;
    for (symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
        if (!chs_buffer_append_u32le(&buffer, symbols[symbol_index].name, error) ||
            !chs_buffer_append_u8(&buffer, symbols[symbol_index].info, error) ||
            !chs_buffer_append_u8(&buffer, symbols[symbol_index].other, error) ||
            !chs_buffer_append_u16le(&buffer, symbols[symbol_index].shndx, error) ||
            !chs_buffer_append_u64le(&buffer, symbols[symbol_index].value, error) ||
            !chs_buffer_append_u64le(&buffer, symbols[symbol_index].size, error)) {
            goto cleanup;
        }
    }

    section_headers[total_section_headers - 2].name_index = (uint32_t) shstrtab.length;
    if (!chs_buffer_append(&shstrtab, ".strtab", 8, error)) {
        goto cleanup;
    }
    section_headers[total_section_headers - 2].type = CHS_SHT_STRTAB;
    section_headers[total_section_headers - 2].alignment = 1u;
    section_headers[total_section_headers - 2].offset = buffer.length;
    section_headers[total_section_headers - 2].size = strtab.length;
    if (!chs_buffer_append(&buffer, strtab.data, strtab.length, error)) {
        goto cleanup;
    }

    section_headers[total_section_headers - 1].name_index = (uint32_t) shstrtab.length;
    if (!chs_buffer_append(&shstrtab, ".shstrtab", 10, error)) {
        goto cleanup;
    }
    section_headers[total_section_headers - 1].type = CHS_SHT_STRTAB;
    section_headers[total_section_headers - 1].alignment = 1u;
    section_headers[total_section_headers - 1].offset = buffer.length;
    section_headers[total_section_headers - 1].size = shstrtab.length;
    if (!chs_buffer_append(&buffer, shstrtab.data, shstrtab.length, error)) {
        goto cleanup;
    }

    while ((buffer.length & 7u) != 0) {
        if (!chs_buffer_append_u8(&buffer, 0u, error)) {
            goto cleanup;
        }
    }
    section_header_offset = buffer.length;

    for (section_index = 0; section_index < total_section_headers; ++section_index) {
        if (!chs_buffer_append_u32le(&buffer, section_headers[section_index].name_index, error) ||
            !chs_buffer_append_u32le(&buffer, section_headers[section_index].type, error) ||
            !chs_buffer_append_u64le(&buffer, section_headers[section_index].flags, error) ||
            !chs_buffer_append_u64le(&buffer, 0u, error) ||
            !chs_buffer_append_u64le(&buffer, section_headers[section_index].offset, error) ||
            !chs_buffer_append_u64le(&buffer, section_headers[section_index].size, error) ||
            !chs_buffer_append_u32le(&buffer, section_headers[section_index].link, error) ||
            !chs_buffer_append_u32le(&buffer, section_headers[section_index].info, error) ||
            !chs_buffer_append_u64le(&buffer, section_headers[section_index].alignment, error) ||
            !chs_buffer_append_u64le(&buffer, section_headers[section_index].entry_size, error)) {
            goto cleanup;
        }
    }

    buffer.data[40] = (uint8_t) (section_header_offset & 0xffu);
    buffer.data[41] = (uint8_t) ((section_header_offset >> 8) & 0xffu);
    buffer.data[42] = (uint8_t) ((section_header_offset >> 16) & 0xffu);
    buffer.data[43] = (uint8_t) ((section_header_offset >> 24) & 0xffu);
    buffer.data[44] = (uint8_t) ((section_header_offset >> 32) & 0xffu);
    buffer.data[45] = (uint8_t) ((section_header_offset >> 40) & 0xffu);
    buffer.data[46] = (uint8_t) ((section_header_offset >> 48) & 0xffu);
    buffer.data[47] = (uint8_t) ((section_header_offset >> 56) & 0xffu);

    if (!chs_write_entire_file(output_path, buffer.data, buffer.length, error)) {
        goto cleanup;
    }

    success = true;

cleanup:
    chs_buffer_free(&buffer);
    chs_buffer_free(&shstrtab);
    chs_buffer_free(&strtab);
    free(section_headers);
    free(symbols);
    free(symbol_order);
    free(symbol_index_map);
    return success;
}