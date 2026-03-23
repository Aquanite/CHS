#include "chs/arch.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int code;
    int width;
    bool is_xmm;
    bool is_rip;
} ChsX86Reg;

typedef struct {
    bool has_base;
    ChsX86Reg base;
    int64_t disp;
    bool has_symbol;
    char symbol[128];
    int width;
} ChsX86Mem;

typedef enum {
    CHS_X86_OP_NONE,
    CHS_X86_OP_REG,
    CHS_X86_OP_MEM,
    CHS_X86_OP_IMM,
    CHS_X86_OP_LABEL
} ChsX86OperandKind;

typedef struct {
    ChsX86OperandKind kind;
    ChsX86Reg reg;
    ChsX86Mem mem;
    int64_t imm;
    char label[128];
} ChsX86Operand;

static void chs_x86_trim_copy(char *out, size_t out_size, const char *in) {
    size_t length;

    while (*in != '\0' && isspace((unsigned char) *in)) {
        ++in;
    }
    length = strlen(in);
    while (length > 0 && isspace((unsigned char) in[length - 1])) {
        --length;
    }
    if (length >= out_size) {
        length = out_size - 1;
    }
    memcpy(out, in, length);
    out[length] = '\0';
}

static size_t chs_x86_split_operands(const char *text, char operands[3][128]) {
    size_t count;
    size_t depth;
    const char *start;
    const char *cursor;

    count = 0;
    depth = 0;
    start = text;
    cursor = text;
    while (*cursor != '\0') {
        if (*cursor == '[') {
            ++depth;
        } else if (*cursor == ']') {
            if (depth > 0) {
                --depth;
            }
        } else if (*cursor == ',' && depth == 0) {
            if (count < 3) {
                size_t length = (size_t) (cursor - start);
                if (length >= sizeof(operands[count])) {
                    length = sizeof(operands[count]) - 1;
                }
                memcpy(operands[count], start, length);
                operands[count][length] = '\0';
                chs_x86_trim_copy(operands[count], sizeof(operands[count]), operands[count]);
                ++count;
            }
            start = cursor + 1;
        }
        ++cursor;
    }

    if (*start != '\0' && count < 3) {
        size_t length = (size_t) (cursor - start);
        if (length >= sizeof(operands[count])) {
            length = sizeof(operands[count]) - 1;
        }
        memcpy(operands[count], start, length);
        operands[count][length] = '\0';
        chs_x86_trim_copy(operands[count], sizeof(operands[count]), operands[count]);
        ++count;
    }

    return count;
}

static bool chs_x86_parse_reg(const char *text, ChsX86Reg *reg) {
    struct {
        const char *name;
        int code;
        int width;
        bool is_xmm;
        bool is_rip;
    } table[] = {
        {"rax", 0, 64, false, false}, {"rcx", 1, 64, false, false}, {"rdx", 2, 64, false, false}, {"rbx", 3, 64, false, false},
        {"rsp", 4, 64, false, false}, {"rbp", 5, 64, false, false}, {"rsi", 6, 64, false, false}, {"rdi", 7, 64, false, false},
        {"r8", 8, 64, false, false}, {"r9", 9, 64, false, false}, {"r10", 10, 64, false, false}, {"r11", 11, 64, false, false},
        {"r12", 12, 64, false, false}, {"r13", 13, 64, false, false}, {"r14", 14, 64, false, false}, {"r15", 15, 64, false, false},
        {"eax", 0, 32, false, false}, {"ecx", 1, 32, false, false}, {"edx", 2, 32, false, false}, {"ebx", 3, 32, false, false},
        {"esp", 4, 32, false, false}, {"ebp", 5, 32, false, false}, {"esi", 6, 32, false, false}, {"edi", 7, 32, false, false},
        {"r8d", 8, 32, false, false}, {"r9d", 9, 32, false, false}, {"r10d", 10, 32, false, false}, {"r11d", 11, 32, false, false},
        {"r12d", 12, 32, false, false}, {"r13d", 13, 32, false, false}, {"r14d", 14, 32, false, false}, {"r15d", 15, 32, false, false},
        {"ax", 0, 16, false, false}, {"cx", 1, 16, false, false}, {"dx", 2, 16, false, false}, {"bx", 3, 16, false, false},
        {"sp", 4, 16, false, false}, {"bp", 5, 16, false, false}, {"si", 6, 16, false, false}, {"di", 7, 16, false, false},
        {"r8w", 8, 16, false, false}, {"r9w", 9, 16, false, false}, {"r10w", 10, 16, false, false}, {"r11w", 11, 16, false, false},
        {"r12w", 12, 16, false, false}, {"r13w", 13, 16, false, false}, {"r14w", 14, 16, false, false}, {"r15w", 15, 16, false, false},
        {"al", 0, 8, false, false}, {"cl", 1, 8, false, false}, {"dl", 2, 8, false, false}, {"bl", 3, 8, false, false},
        {"spl", 4, 8, false, false}, {"bpl", 5, 8, false, false}, {"sil", 6, 8, false, false}, {"dil", 7, 8, false, false},
        {"r8b", 8, 8, false, false}, {"r9b", 9, 8, false, false}, {"r10b", 10, 8, false, false}, {"r11b", 11, 8, false, false},
        {"r12b", 12, 8, false, false}, {"r13b", 13, 8, false, false}, {"r14b", 14, 8, false, false}, {"r15b", 15, 8, false, false},
        {"xmm0", 0, 64, true, false}, {"xmm1", 1, 64, true, false}, {"xmm2", 2, 64, true, false}, {"xmm3", 3, 64, true, false},
        {"xmm4", 4, 64, true, false}, {"xmm5", 5, 64, true, false}, {"xmm6", 6, 64, true, false}, {"xmm7", 7, 64, true, false},
        {"xmm8", 8, 64, true, false}, {"xmm9", 9, 64, true, false}, {"xmm10", 10, 64, true, false}, {"xmm11", 11, 64, true, false},
        {"xmm12", 12, 64, true, false}, {"xmm13", 13, 64, true, false}, {"xmm14", 14, 64, true, false}, {"xmm15", 15, 64, true, false},
        {"rip", 0, 64, false, true}
    };
    char name[32];
    size_t i;

    chs_x86_trim_copy(name, sizeof(name), text);
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcmp(name, table[i].name) == 0) {
            reg->code = table[i].code;
            reg->width = table[i].width;
            reg->is_xmm = table[i].is_xmm;
            reg->is_rip = table[i].is_rip;
            return true;
        }
    }
    return false;
}

static bool chs_x86_parse_mem(const char *text, int forced_width, ChsX86Mem *mem, ChsError *error) {
    char copy[192];
    char *left;
    char *right;
    char expr[160];
    const char *cursor;

    memset(mem, 0, sizeof(*mem));
    mem->width = forced_width;
    chs_x86_trim_copy(copy, sizeof(copy), text);
    left = strchr(copy, '[');
    right = strrchr(copy, ']');
    if (left == NULL || right == NULL || right <= left) {
        return false;
    }
    *right = '\0';
    chs_x86_trim_copy(expr, sizeof(expr), left + 1);

    cursor = expr;
    while (*cursor != '\0') {
        char token[128];
        size_t token_len;
        int sign;

        while (*cursor != '\0' && isspace((unsigned char) *cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        sign = 1;
        if (*cursor == '+') {
            ++cursor;
        } else if (*cursor == '-') {
            sign = -1;
            ++cursor;
        }

        while (*cursor != '\0' && isspace((unsigned char) *cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        token_len = 0;
        while (*cursor != '\0' && !isspace((unsigned char) *cursor) && *cursor != '+' && *cursor != '-') {
            if (token_len + 1 < sizeof(token)) {
                token[token_len++] = *cursor;
            }
            ++cursor;
        }
        token[token_len] = '\0';

        if (token_len == 0) {
            continue;
        }

        {
            ChsX86Reg reg;
            int64_t value;
            if (chs_x86_parse_reg(token, &reg)) {
                if (sign < 0) {
                    chs_set_error(error, "x86 memory operand does not support negated register terms");
                    return false;
                }
                mem->has_base = true;
                mem->base = reg;
            } else if (chs_parse_i64(token, &value)) {
                mem->disp += (sign < 0) ? -value : value;
            } else {
                if (mem->has_symbol) {
                    chs_set_error(error, "x86 memory operand supports at most one symbol");
                    return false;
                }
                if (sign < 0) {
                    chs_set_error(error, "x86 memory operand does not support negated symbol terms");
                    return false;
                }
                mem->has_symbol = true;
                snprintf(mem->symbol, sizeof(mem->symbol), "%s", token);
            }
        }
    }

    if (mem->width == 0) {
        mem->width = 8;
    }
    return true;
}

static bool chs_x86_parse_operand(const char *text, ChsX86Operand *operand, ChsError *error) {
    char copy[192];
    char *mem_text;
    int forced_width;

    memset(operand, 0, sizeof(*operand));
    forced_width = 0;
    chs_x86_trim_copy(copy, sizeof(copy), text);

    if (strncmp(copy, "byte ptr ", 9) == 0) {
        forced_width = 1;
        mem_text = copy + 9;
    } else if (strncmp(copy, "word ptr ", 9) == 0) {
        forced_width = 2;
        mem_text = copy + 9;
    } else if (strncmp(copy, "dword ptr ", 10) == 0) {
        forced_width = 4;
        mem_text = copy + 10;
    } else if (strncmp(copy, "qword ptr ", 10) == 0) {
        forced_width = 8;
        mem_text = copy + 10;
    } else {
        mem_text = copy;
    }

    if (strchr(mem_text, '[') != NULL) {
        operand->kind = CHS_X86_OP_MEM;
        return chs_x86_parse_mem(mem_text, forced_width, &operand->mem, error);
    }

    if (chs_x86_parse_reg(copy, &operand->reg)) {
        operand->kind = CHS_X86_OP_REG;
        return true;
    }

    if (chs_parse_i64(copy, &operand->imm)) {
        operand->kind = CHS_X86_OP_IMM;
        return true;
    }

    operand->kind = CHS_X86_OP_LABEL;
    snprintf(operand->label, sizeof(operand->label), "%s", copy);
    return true;
}

static bool chs_x86_put_u8(ChsEncodedInstruction *encoded, uint8_t value) {
    if (encoded->size >= sizeof(encoded->bytes)) {
        return false;
    }
    encoded->bytes[encoded->size++] = value;
    return true;
}

static bool chs_x86_put_u32(ChsEncodedInstruction *encoded, uint32_t value) {
    return chs_x86_put_u8(encoded, (uint8_t) (value & 0xffu)) &&
           chs_x86_put_u8(encoded, (uint8_t) ((value >> 8) & 0xffu)) &&
           chs_x86_put_u8(encoded, (uint8_t) ((value >> 16) & 0xffu)) &&
           chs_x86_put_u8(encoded, (uint8_t) ((value >> 24) & 0xffu));
}

static bool chs_x86_put_u64(ChsEncodedInstruction *encoded, uint64_t value) {
    int i;
    for (i = 0; i < 8; ++i) {
        if (!chs_x86_put_u8(encoded, (uint8_t) ((value >> (i * 8)) & 0xffu))) {
            return false;
        }
    }
    return true;
}

static bool chs_x86_emit_rex(ChsEncodedInstruction *encoded, bool w, int r, int x, int b) {
    uint8_t rex;
    rex = 0x40u;
    if (w) rex |= 0x08u;
    if ((r & 8) != 0) rex |= 0x04u;
    if ((x & 8) != 0) rex |= 0x02u;
    if ((b & 8) != 0) rex |= 0x01u;
    if (rex != 0x40u || w) {
        return chs_x86_put_u8(encoded, rex);
    }
    return true;
}

static bool chs_x86_emit_modrm(ChsEncodedInstruction *encoded, uint8_t mod, uint8_t reg, uint8_t rm) {
    return chs_x86_put_u8(encoded, (uint8_t) (((mod & 0x3u) << 6) | ((reg & 0x7u) << 3) | (rm & 0x7u)));
}

static bool chs_x86_emit_rm(ChsEncodedInstruction *encoded,
                            uint8_t reg_field,
                            const ChsX86Operand *rm_operand,
                            uint8_t *reloc_offset,
                            bool *has_reloc,
                            char reloc_symbol[128],
                            ChsError *error) {
    if (rm_operand->kind == CHS_X86_OP_REG) {
        return chs_x86_emit_modrm(encoded, 3, reg_field, (uint8_t) (rm_operand->reg.code & 7));
    }

    if (rm_operand->kind != CHS_X86_OP_MEM) {
        chs_set_error(error, "x86 expected register or memory operand");
        return false;
    }

    {
        const ChsX86Mem *mem = &rm_operand->mem;
        uint8_t mod;
        uint8_t rm;
        bool need_sib;
        int64_t disp;
        bool force_disp32;

        need_sib = false;
        disp = mem->disp;
        force_disp32 = mem->has_symbol;

        if (mem->has_base && mem->base.is_rip) {
            mod = 0;
            rm = 5;
            if (!chs_x86_emit_modrm(encoded, mod, reg_field, rm)) {
                return false;
            }
            *reloc_offset = (uint8_t) encoded->size;
            if (!chs_x86_put_u32(encoded, (uint32_t) disp)) {
                return false;
            }
            if (mem->has_symbol) {
                *has_reloc = true;
                snprintf(reloc_symbol, 128, "%s", mem->symbol);
            }
            return true;
        }

        if (!mem->has_base) {
            mod = 0;
            rm = 5;
            if (!chs_x86_emit_modrm(encoded, mod, reg_field, rm)) {
                return false;
            }
            *reloc_offset = (uint8_t) encoded->size;
            if (!chs_x86_put_u32(encoded, (uint32_t) disp)) {
                return false;
            }
            if (mem->has_symbol) {
                *has_reloc = true;
                snprintf(reloc_symbol, 128, "%s", mem->symbol);
            }
            return true;
        }

        rm = (uint8_t) (mem->base.code & 7);
        need_sib = (rm == 4);

        if (force_disp32) {
            mod = 2;
        } else if (disp == 0 && rm != 5) {
            mod = 0;
        } else if (disp >= -128 && disp <= 127) {
            mod = 1;
        } else {
            mod = 2;
        }

        if (disp == 0 && rm == 5 && !force_disp32) {
            mod = 1;
        }

        if (!chs_x86_emit_modrm(encoded, mod, reg_field, need_sib ? 4 : rm)) {
            return false;
        }
        if (need_sib) {
            if (!chs_x86_put_u8(encoded, (uint8_t) ((0u << 6) | (4u << 3) | rm))) {
                return false;
            }
        }

        if (mod == 1) {
            if (!chs_x86_put_u8(encoded, (uint8_t) disp)) {
                return false;
            }
        } else if (mod == 2 || (mod == 0 && rm == 5)) {
            *reloc_offset = (uint8_t) encoded->size;
            if (!chs_x86_put_u32(encoded, (uint32_t) disp)) {
                return false;
            }
            if (mem->has_symbol) {
                *has_reloc = true;
                snprintf(reloc_symbol, 128, "%s", mem->symbol);
            }
        }
        return true;
    }
}

static bool chs_x86_lookup_cc(const char *mnemonic, bool is_setcc, uint8_t *cc) {
    const char *name;

    name = mnemonic;
    if (is_setcc) {
        if (strncmp(mnemonic, "set", 3) != 0) {
            return false;
        }
        name = mnemonic + 3;
    } else {
        if (strncmp(mnemonic, "j", 1) != 0 || strcmp(mnemonic, "jmp") == 0) {
            return false;
        }
        name = mnemonic + 1;
    }

    if (strcmp(name, "e") == 0 || strcmp(name, "z") == 0) *cc = 0x4;
    else if (strcmp(name, "ne") == 0 || strcmp(name, "nz") == 0) *cc = 0x5;
    else if (strcmp(name, "l") == 0 || strcmp(name, "nge") == 0) *cc = 0xC;
    else if (strcmp(name, "le") == 0 || strcmp(name, "ng") == 0) *cc = 0xE;
    else if (strcmp(name, "g") == 0 || strcmp(name, "nle") == 0) *cc = 0xF;
    else if (strcmp(name, "ge") == 0 || strcmp(name, "nl") == 0) *cc = 0xD;
    else if (strcmp(name, "b") == 0 || strcmp(name, "c") == 0 || strcmp(name, "nae") == 0) *cc = 0x2;
    else if (strcmp(name, "be") == 0 || strcmp(name, "na") == 0) *cc = 0x6;
    else if (strcmp(name, "a") == 0 || strcmp(name, "nbe") == 0) *cc = 0x7;
    else if (strcmp(name, "ae") == 0 || strcmp(name, "nb") == 0 || strcmp(name, "nc") == 0) *cc = 0x3;
    else return false;

    return true;
}

static bool chs_x86_encode_mov(const ChsX86Operand *dst,
                               const ChsX86Operand *src,
                               ChsEncodedInstruction *encoded,
                               ChsError *error) {
    if (dst->kind == CHS_X86_OP_REG && src->kind == CHS_X86_OP_IMM) {
        int width = dst->reg.width;
        int reg = dst->reg.code;
        if (width == 64) {
            if (!chs_x86_emit_rex(encoded, true, 0, 0, reg) ||
                !chs_x86_put_u8(encoded, (uint8_t) (0xB8u + (reg & 7))) ||
                !chs_x86_put_u64(encoded, (uint64_t) src->imm)) {
                return false;
            }
            return true;
        }
        if (width == 32) {
            if (!chs_x86_emit_rex(encoded, false, 0, 0, reg) ||
                !chs_x86_put_u8(encoded, (uint8_t) (0xB8u + (reg & 7))) ||
                !chs_x86_put_u32(encoded, (uint32_t) src->imm)) {
                return false;
            }
            return true;
        }
        if (width == 16) {
            if (!chs_x86_put_u8(encoded, 0x66u) ||
                !chs_x86_emit_rex(encoded, false, 0, 0, reg) ||
                !chs_x86_put_u8(encoded, (uint8_t) (0xB8u + (reg & 7))) ||
                !chs_x86_put_u8(encoded, (uint8_t) (src->imm & 0xff)) ||
                !chs_x86_put_u8(encoded, (uint8_t) ((src->imm >> 8) & 0xff))) {
                return false;
            }
            return true;
        }
        chs_set_error(error, "x86 mov immediate currently supports 32/64-bit destinations");
        return false;
    }

    if ((dst->kind == CHS_X86_OP_REG || dst->kind == CHS_X86_OP_MEM) &&
        (src->kind == CHS_X86_OP_REG || src->kind == CHS_X86_OP_MEM)) {
        bool reg_from_rm;
        const ChsX86Operand *reg_op;
        const ChsX86Operand *rm_op;
        int width;
        uint8_t reloc_offset;
        bool has_reloc;
        char reloc_symbol[128];

        reloc_offset = 0;
        has_reloc = false;
        reloc_symbol[0] = '\0';

        reg_from_rm = (dst->kind == CHS_X86_OP_REG);
        reg_op = reg_from_rm ? dst : src;
        rm_op = reg_from_rm ? src : dst;
        width = reg_op->reg.width;

        if (width == 64) {
            if (!chs_x86_emit_rex(encoded, true, reg_op->reg.code, 0, (rm_op->kind == CHS_X86_OP_REG) ? rm_op->reg.code : (rm_op->mem.has_base ? rm_op->mem.base.code : 0)) ||
                !chs_x86_put_u8(encoded, (uint8_t) (reg_from_rm ? 0x8Bu : 0x89u))) {
                return false;
            }
            if (!chs_x86_emit_rm(encoded, (uint8_t) reg_op->reg.code, rm_op, &reloc_offset, &has_reloc, reloc_symbol, error)) {
                return false;
            }
        } else if (width == 16) {
            if (!chs_x86_put_u8(encoded, 0x66u) ||
                !chs_x86_emit_rex(encoded, false, reg_op->reg.code, 0, (rm_op->kind == CHS_X86_OP_REG) ? rm_op->reg.code : (rm_op->mem.has_base ? rm_op->mem.base.code : 0)) ||
                !chs_x86_put_u8(encoded, (uint8_t) (reg_from_rm ? 0x8Bu : 0x89u))) {
                return false;
            }
            if (!chs_x86_emit_rm(encoded, (uint8_t) reg_op->reg.code, rm_op, &reloc_offset, &has_reloc, reloc_symbol, error)) {
                return false;
            }
        } else if (width == 32) {
            if (!chs_x86_emit_rex(encoded, false, reg_op->reg.code, 0, (rm_op->kind == CHS_X86_OP_REG) ? rm_op->reg.code : (rm_op->mem.has_base ? rm_op->mem.base.code : 0)) ||
                !chs_x86_put_u8(encoded, (uint8_t) (reg_from_rm ? 0x8Bu : 0x89u))) {
                return false;
            }
            if (!chs_x86_emit_rm(encoded, (uint8_t) reg_op->reg.code, rm_op, &reloc_offset, &has_reloc, reloc_symbol, error)) {
                return false;
            }
        } else if (width == 8) {
            if (!chs_x86_emit_rex(encoded, false, reg_op->reg.code, 0, (rm_op->kind == CHS_X86_OP_REG) ? rm_op->reg.code : (rm_op->mem.has_base ? rm_op->mem.base.code : 0)) ||
                !chs_x86_put_u8(encoded, (uint8_t) (reg_from_rm ? 0x8Au : 0x88u))) {
                return false;
            }
            if (!chs_x86_emit_rm(encoded, (uint8_t) reg_op->reg.code, rm_op, &reloc_offset, &has_reloc, reloc_symbol, error)) {
                return false;
            }
        } else {
            chs_set_error(error, "x86 mov width %d is not supported", width);
            return false;
        }

        if (has_reloc) {
            encoded->uses_symbol = true;
            encoded->symbol_name = chs_strdup(reloc_symbol, error);
            if (encoded->symbol_name == NULL) {
                return false;
            }
            encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
            encoded->pc_relative = true;
            encoded->relocation_offset = reloc_offset;
            encoded->relocation_addend = 0;
        }
        return true;
    }

    if (dst->kind == CHS_X86_OP_MEM && src->kind == CHS_X86_OP_REG && src->reg.is_xmm) {
        uint8_t reloc_offset = 0;
        bool has_reloc = false;
        char reloc_symbol[128] = {0};
        if (!chs_x86_put_u8(encoded, 0xF2u) ||
            !chs_x86_emit_rex(encoded, false, src->reg.code, 0, dst->mem.has_base ? dst->mem.base.code : 0) ||
            !chs_x86_put_u8(encoded, 0x0Fu) ||
            !chs_x86_put_u8(encoded, 0x11u)) {
            return false;
        }
        if (!chs_x86_emit_rm(encoded, (uint8_t) src->reg.code, dst, &reloc_offset, &has_reloc, reloc_symbol, error)) {
            return false;
        }
        if (has_reloc) {
            encoded->uses_symbol = true;
            encoded->symbol_name = chs_strdup(reloc_symbol, error);
            if (encoded->symbol_name == NULL) {
                return false;
            }
            encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
            encoded->pc_relative = true;
            encoded->relocation_offset = reloc_offset;
            encoded->relocation_addend = 0;
        }
        return true;
    }

    if (dst->kind == CHS_X86_OP_REG && dst->reg.is_xmm && src->kind == CHS_X86_OP_MEM) {
        uint8_t reloc_offset = 0;
        bool has_reloc = false;
        char reloc_symbol[128] = {0};
        if (!chs_x86_put_u8(encoded, 0xF2u) ||
            !chs_x86_emit_rex(encoded, false, dst->reg.code, 0, src->mem.has_base ? src->mem.base.code : 0) ||
            !chs_x86_put_u8(encoded, 0x0Fu) ||
            !chs_x86_put_u8(encoded, 0x10u)) {
            return false;
        }
        if (!chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
            return false;
        }
        if (has_reloc) {
            encoded->uses_symbol = true;
            encoded->symbol_name = chs_strdup(reloc_symbol, error);
            if (encoded->symbol_name == NULL) {
                return false;
            }
            encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
            encoded->pc_relative = true;
            encoded->relocation_offset = reloc_offset;
            encoded->relocation_addend = 0;
        }
        return true;
    }

    chs_set_error(error, "unsupported x86 mov operand form");
    return false;
}

static bool chs_x86_encode_push_pop(const char *mnemonic,
                                    const ChsX86Operand *operand,
                                    ChsEncodedInstruction *encoded,
                                    ChsError *error) {
    uint8_t opcode_base;
    if (operand->kind != CHS_X86_OP_REG || operand->reg.width != 64) {
        chs_set_error(error, "%s expects a 64-bit register", mnemonic);
        return false;
    }
    opcode_base = (strcmp(mnemonic, "push") == 0) ? 0x50u : 0x58u;
    if (!chs_x86_emit_rex(encoded, false, 0, 0, operand->reg.code) ||
        !chs_x86_put_u8(encoded, (uint8_t) (opcode_base + (operand->reg.code & 7)))) {
        return false;
    }
    return true;
}

static bool chs_x86_encode_branch_label(const char *mnemonic,
                                        const ChsX86Operand *operand,
                                        ChsEncodedInstruction *encoded,
                                        ChsError *error) {
    uint8_t cc;

    if (strcmp(mnemonic, "call") == 0 || strcmp(mnemonic, "jmp") == 0) {
        if (operand->kind == CHS_X86_OP_REG || operand->kind == CHS_X86_OP_MEM) {
            uint8_t reloc_offset = 0;
            bool has_reloc = false;
            char reloc_symbol[128] = {0};
            uint8_t ext = (strcmp(mnemonic, "call") == 0) ? 2u : 4u;
            int b;

            if (operand->kind == CHS_X86_OP_REG && operand->reg.width != 64) {
                chs_set_error(error, "%s expects a 64-bit register operand", mnemonic);
                return false;
            }
            b = (operand->kind == CHS_X86_OP_REG) ? operand->reg.code : (operand->mem.has_base ? operand->mem.base.code : 0);
            if (!chs_x86_emit_rex(encoded, true, 0, 0, b) || !chs_x86_put_u8(encoded, 0xFFu)) {
                return false;
            }
            if (!chs_x86_emit_rm(encoded, ext, operand, &reloc_offset, &has_reloc, reloc_symbol, error)) {
                return false;
            }
            if (has_reloc) {
                encoded->uses_symbol = true;
                encoded->symbol_name = chs_strdup(reloc_symbol, error);
                if (encoded->symbol_name == NULL) {
                    return false;
                }
                encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
                encoded->pc_relative = true;
                encoded->relocation_offset = reloc_offset;
                encoded->relocation_addend = 0;
            }
            return true;
        }
        if (operand->kind != CHS_X86_OP_LABEL) {
            chs_set_error(error, "%s expects a label, register, or memory operand", mnemonic);
            return false;
        }
    }

    if (operand->kind != CHS_X86_OP_LABEL) {
        chs_set_error(error, "%s expects a label operand", mnemonic);
        return false;
    }

    if (strcmp(mnemonic, "call") == 0) {
        if (!chs_x86_put_u8(encoded, 0xE8u) || !chs_x86_put_u32(encoded, 0u)) {
            return false;
        }
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(operand->label, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_BRANCH32;
        encoded->pc_relative = true;
        encoded->relocation_offset = 1;
        encoded->relocation_addend = 0;
        return true;
    }

    if (strcmp(mnemonic, "jmp") == 0) {
        if (!chs_x86_put_u8(encoded, 0xE9u) || !chs_x86_put_u32(encoded, 0u)) {
            return false;
        }
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(operand->label, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_BRANCH32;
        encoded->pc_relative = true;
        encoded->relocation_offset = 1;
        encoded->relocation_addend = 0;
        return true;
    }

    if (!chs_x86_lookup_cc(mnemonic, false, &cc)) {
        chs_set_error(error, "unsupported jump mnemonic %s", mnemonic);
        return false;
    }
    if (!chs_x86_put_u8(encoded, 0x0Fu) || !chs_x86_put_u8(encoded, (uint8_t) (0x80u + cc)) || !chs_x86_put_u32(encoded, 0u)) {
        return false;
    }
    encoded->uses_symbol = true;
    encoded->symbol_name = chs_strdup(operand->label, error);
    if (encoded->symbol_name == NULL) {
        return false;
    }
    encoded->relocation_kind = CHS_RELOC_X86_64_BRANCH32;
    encoded->pc_relative = true;
    encoded->relocation_offset = 2;
    encoded->relocation_addend = 0;
    return true;
}

static bool chs_x86_encode_setcc(const char *mnemonic,
                                 const ChsX86Operand *operand,
                                 ChsEncodedInstruction *encoded,
                                 ChsError *error) {
    uint8_t cc;
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};

    if (!chs_x86_lookup_cc(mnemonic, true, &cc)) {
        chs_set_error(error, "unsupported setcc mnemonic %s", mnemonic);
        return false;
    }

    if (!chs_x86_put_u8(encoded, 0x0Fu) || !chs_x86_put_u8(encoded, (uint8_t) (0x90u + cc))) {
        return false;
    }
    if (!chs_x86_emit_rm(encoded, 0, operand, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }
    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }
    return true;
}

static bool chs_x86_encode_lea(const ChsX86Operand *dst,
                               const ChsX86Operand *src,
                               ChsEncodedInstruction *encoded,
                               ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};
    if (dst->kind != CHS_X86_OP_REG || src->kind != CHS_X86_OP_MEM || dst->reg.width != 64) {
        chs_set_error(error, "lea expects reg64, mem operand");
        return false;
    }
    if (!chs_x86_emit_rex(encoded, true, dst->reg.code, 0, src->mem.has_base ? src->mem.base.code : 0) ||
        !chs_x86_put_u8(encoded, 0x8Du)) {
        return false;
    }
    if (!chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }
    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }
    return true;
}

static bool chs_x86_encode_binop_rrm(const char *mnemonic,
                                     const ChsX86Operand *dst,
                                     const ChsX86Operand *src,
                                     ChsEncodedInstruction *encoded,
                                     ChsError *error) {
    uint8_t opcode;
    uint8_t ext;
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};

    if (strcmp(mnemonic, "add") == 0) {
        opcode = 0x03u; ext = 0;
    } else if (strcmp(mnemonic, "sub") == 0) {
        opcode = 0x2Bu; ext = 5;
    } else if (strcmp(mnemonic, "cmp") == 0) {
        opcode = 0x3Bu; ext = 7;
    } else if (strcmp(mnemonic, "xor") == 0) {
        opcode = 0x33u; ext = 6;
    } else if (strcmp(mnemonic, "and") == 0) {
        opcode = 0x23u; ext = 4;
    } else if (strcmp(mnemonic, "or") == 0) {
        opcode = 0x0Bu; ext = 1;
    } else {
        chs_set_error(error, "unsupported binary operation %s", mnemonic);
        return false;
    }

    if (dst->kind == CHS_X86_OP_REG && (src->kind == CHS_X86_OP_REG || src->kind == CHS_X86_OP_MEM)) {
        bool w = (dst->reg.width == 64);
        if (!chs_x86_emit_rex(encoded, w, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
            !chs_x86_put_u8(encoded, opcode)) {
            return false;
        }
        if (!chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
            return false;
        }
        if (has_reloc) {
            encoded->uses_symbol = true;
            encoded->symbol_name = chs_strdup(reloc_symbol, error);
            if (encoded->symbol_name == NULL) {
                return false;
            }
            encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
            encoded->pc_relative = true;
            encoded->relocation_offset = reloc_offset;
            encoded->relocation_addend = 0;
        }
        return true;
    }

    if (dst->kind == CHS_X86_OP_REG && src->kind == CHS_X86_OP_IMM) {
        bool w = (dst->reg.width == 64);
        uint8_t opcode_imm;
        int64_t imm = src->imm;
        opcode_imm = (imm >= -128 && imm <= 127) ? 0x83u : 0x81u;
        if (!chs_x86_emit_rex(encoded, w, 0, 0, dst->reg.code) ||
            !chs_x86_put_u8(encoded, opcode_imm) ||
            !chs_x86_emit_modrm(encoded, 3, ext, (uint8_t) dst->reg.code)) {
            return false;
        }
        if (opcode_imm == 0x83u) {
            return chs_x86_put_u8(encoded, (uint8_t) imm);
        }
        return chs_x86_put_u32(encoded, (uint32_t) imm);
    }

    chs_set_error(error, "unsupported %s operand form", mnemonic);
    return false;
}

static bool chs_x86_encode_movsx_family(const char *mnemonic,
                                        const ChsX86Operand *dst,
                                        const ChsX86Operand *src,
                                        ChsEncodedInstruction *encoded,
                                        ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};
    uint8_t op2;

    if (dst->kind != CHS_X86_OP_REG || (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM)) {
        chs_set_error(error, "%s expects reg destination and reg/mem source", mnemonic);
        return false;
    }

    if (strcmp(mnemonic, "movsxd") == 0) {
        if (!chs_x86_emit_rex(encoded, true, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
            !chs_x86_put_u8(encoded, 0x63u)) {
            return false;
        }
        if (!chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
            return false;
        }
    } else {
        if (strcmp(mnemonic, "movzx") == 0) {
            op2 = 0xB6u;
        } else {
            op2 = 0xBEu;
        }
        if (!chs_x86_emit_rex(encoded, false, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
            !chs_x86_put_u8(encoded, 0x0Fu) ||
            !chs_x86_put_u8(encoded, op2)) {
            return false;
        }
        if (!chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
            return false;
        }
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }
    return true;
}

static bool chs_x86_encode_unary_rm(const char *mnemonic,
                                    uint8_t ext,
                                    const ChsX86Operand *operand,
                                    ChsEncodedInstruction *encoded,
                                    ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};
    int width;
    int b;

    if (operand->kind != CHS_X86_OP_REG && operand->kind != CHS_X86_OP_MEM) {
        chs_set_error(error, "%s expects register or memory operand", mnemonic);
        return false;
    }

    width = (operand->kind == CHS_X86_OP_REG) ? operand->reg.width : operand->mem.width * 8;
    if (width != 64 && width != 32) {
        chs_set_error(error, "%s currently supports 32/64-bit operands", mnemonic);
        return false;
    }
    b = (operand->kind == CHS_X86_OP_REG) ? operand->reg.code : (operand->mem.has_base ? operand->mem.base.code : 0);

    if (!chs_x86_emit_rex(encoded, width == 64, 0, 0, b) ||
        !chs_x86_put_u8(encoded, 0xF7u)) {
        return false;
    }
    if (!chs_x86_emit_rm(encoded, ext, operand, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_shift(const char *mnemonic,
                                 const ChsX86Operand *dst,
                                 const ChsX86Operand *src,
                                 ChsEncodedInstruction *encoded,
                                 ChsError *error) {
    uint8_t ext;
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};
    int width;
    int b;

    if (strcmp(mnemonic, "shl") == 0 || strcmp(mnemonic, "sal") == 0) ext = 4;
    else if (strcmp(mnemonic, "shr") == 0) ext = 5;
    else if (strcmp(mnemonic, "sar") == 0) ext = 7;
    else {
        chs_set_error(error, "unsupported shift mnemonic %s", mnemonic);
        return false;
    }

    if (dst->kind != CHS_X86_OP_REG && dst->kind != CHS_X86_OP_MEM) {
        chs_set_error(error, "%s expects register or memory destination", mnemonic);
        return false;
    }

    width = (dst->kind == CHS_X86_OP_REG) ? dst->reg.width : dst->mem.width * 8;
    if (width != 64 && width != 32) {
        chs_set_error(error, "%s currently supports 32/64-bit destinations", mnemonic);
        return false;
    }
    b = (dst->kind == CHS_X86_OP_REG) ? dst->reg.code : (dst->mem.has_base ? dst->mem.base.code : 0);

    if (!chs_x86_emit_rex(encoded, width == 64, 0, 0, b)) {
        return false;
    }

    if (src->kind == CHS_X86_OP_NONE) {
        if (!chs_x86_put_u8(encoded, 0xD1u)) {
            return false;
        }
        return chs_x86_emit_rm(encoded, ext, dst, &reloc_offset, &has_reloc, reloc_symbol, error);
    }

    if (src->kind == CHS_X86_OP_IMM) {
        if (src->imm == 1) {
            if (!chs_x86_put_u8(encoded, 0xD1u)) {
                return false;
            }
            return chs_x86_emit_rm(encoded, ext, dst, &reloc_offset, &has_reloc, reloc_symbol, error);
        }
        if (!chs_x86_put_u8(encoded, 0xC1u) ||
            !chs_x86_emit_rm(encoded, ext, dst, &reloc_offset, &has_reloc, reloc_symbol, error) ||
            !chs_x86_put_u8(encoded, (uint8_t) src->imm)) {
            return false;
        }
        return true;
    }

    if (src->kind == CHS_X86_OP_REG && src->reg.code == 1 && src->reg.width == 8) {
        if (!chs_x86_put_u8(encoded, 0xD3u)) {
            return false;
        }
        return chs_x86_emit_rm(encoded, ext, dst, &reloc_offset, &has_reloc, reloc_symbol, error);
    }

    chs_set_error(error, "%s unsupported count operand", mnemonic);
    return false;
}

static bool chs_x86_encode_imul(const ChsX86Operand *dst,
                                const ChsX86Operand *src,
                                ChsEncodedInstruction *encoded,
                                ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};

    if (dst->kind != CHS_X86_OP_REG || (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM)) {
        chs_set_error(error, "imul expects reg destination and reg/mem source");
        return false;
    }

    if (!chs_x86_emit_rex(encoded,
                          dst->reg.width == 64,
                          dst->reg.code,
                          0,
                          (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
        !chs_x86_put_u8(encoded, 0x0Fu) ||
        !chs_x86_put_u8(encoded, 0xAFu)) {
        return false;
    }

    if (!chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_movq(const ChsX86Operand *dst,
                                const ChsX86Operand *src,
                                ChsEncodedInstruction *encoded,
                                ChsError *error) {
    if (dst->kind == CHS_X86_OP_REG && dst->reg.is_xmm && src->kind == CHS_X86_OP_REG && !src->reg.is_xmm && src->reg.width == 64) {
        if (!chs_x86_put_u8(encoded, 0x66u) ||
            !chs_x86_emit_rex(encoded, true, dst->reg.code, 0, src->reg.code) ||
            !chs_x86_put_u8(encoded, 0x0Fu) ||
            !chs_x86_put_u8(encoded, 0x6Eu) ||
            !chs_x86_emit_modrm(encoded, 3, (uint8_t) dst->reg.code, (uint8_t) src->reg.code)) {
            return false;
        }
        return true;
    }

    if (dst->kind == CHS_X86_OP_REG && !dst->reg.is_xmm && dst->reg.width == 64 && src->kind == CHS_X86_OP_REG && src->reg.is_xmm) {
        if (!chs_x86_put_u8(encoded, 0x66u) ||
            !chs_x86_emit_rex(encoded, true, src->reg.code, 0, dst->reg.code) ||
            !chs_x86_put_u8(encoded, 0x0Fu) ||
            !chs_x86_put_u8(encoded, 0x7Eu) ||
            !chs_x86_emit_modrm(encoded, 3, (uint8_t) src->reg.code, (uint8_t) dst->reg.code)) {
            return false;
        }
        return true;
    }

    chs_set_error(error, "unsupported movq operand form");
    return false;
}

static bool chs_x86_encode_movd(const ChsX86Operand *dst,
                                const ChsX86Operand *src,
                                ChsEncodedInstruction *encoded,
                                ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};

    if (dst->kind == CHS_X86_OP_REG && dst->reg.is_xmm && (src->kind == CHS_X86_OP_REG || src->kind == CHS_X86_OP_MEM)) {
        int b;
        if (src->kind == CHS_X86_OP_REG) {
            if (src->reg.is_xmm || src->reg.width != 32) {
                chs_set_error(error, "movd expects r32 source when destination is xmm");
                return false;
            }
            b = src->reg.code;
        } else {
            if (src->mem.width != 4) {
                chs_set_error(error, "movd xmm destination expects m32 source");
                return false;
            }
            b = src->mem.has_base ? src->mem.base.code : 0;
        }

        if (!chs_x86_put_u8(encoded, 0x66u) ||
            !chs_x86_emit_rex(encoded, false, dst->reg.code, 0, b) ||
            !chs_x86_put_u8(encoded, 0x0Fu) ||
            !chs_x86_put_u8(encoded, 0x6Eu) ||
            !chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
            return false;
        }
    } else if ((dst->kind == CHS_X86_OP_REG || dst->kind == CHS_X86_OP_MEM) && src->kind == CHS_X86_OP_REG && src->reg.is_xmm) {
        int b = (dst->kind == CHS_X86_OP_REG) ? dst->reg.code : (dst->mem.has_base ? dst->mem.base.code : 0);

        if (dst->kind == CHS_X86_OP_REG) {
            if (dst->reg.is_xmm || dst->reg.width != 32) {
                chs_set_error(error, "movd expects r32 destination when source is xmm");
                return false;
            }
        } else if (dst->mem.width != 4) {
            chs_set_error(error, "movd xmm source expects m32 destination");
            return false;
        }

        if (!chs_x86_put_u8(encoded, 0x66u) ||
            !chs_x86_emit_rex(encoded, false, src->reg.code, 0, b) ||
            !chs_x86_put_u8(encoded, 0x0Fu) ||
            !chs_x86_put_u8(encoded, 0x7Eu) ||
            !chs_x86_emit_rm(encoded, (uint8_t) src->reg.code, dst, &reloc_offset, &has_reloc, reloc_symbol, error)) {
            return false;
        }
    } else {
        chs_set_error(error, "unsupported movd operand form");
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_cmpsd(const ChsX86Operand *dst,
                                 const ChsX86Operand *src,
                                 const ChsX86Operand *imm,
                                 ChsEncodedInstruction *encoded,
                                 ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};

    if (dst->kind != CHS_X86_OP_REG || !dst->reg.is_xmm) {
        chs_set_error(error, "cmpsd expects xmm destination");
        return false;
    }
    if (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM) {
        chs_set_error(error, "cmpsd expects xmm/m64 source");
        return false;
    }
    if (imm->kind != CHS_X86_OP_IMM) {
        chs_set_error(error, "cmpsd expects an immediate predicate operand");
        return false;
    }

    if (!chs_x86_put_u8(encoded, 0xF2u) ||
        !chs_x86_emit_rex(encoded, false, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
        !chs_x86_put_u8(encoded, 0x0Fu) ||
        !chs_x86_put_u8(encoded, 0xC2u) ||
        !chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error) ||
        !chs_x86_put_u8(encoded, (uint8_t) imm->imm)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_cmpss(const ChsX86Operand *dst,
                                 const ChsX86Operand *src,
                                 const ChsX86Operand *imm,
                                 ChsEncodedInstruction *encoded,
                                 ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};

    if (dst->kind != CHS_X86_OP_REG || !dst->reg.is_xmm) {
        chs_set_error(error, "cmpss expects xmm destination");
        return false;
    }
    if (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM) {
        chs_set_error(error, "cmpss expects xmm/m32 source");
        return false;
    }
    if (src->kind == CHS_X86_OP_MEM && src->mem.width != 0 && src->mem.width != 4) {
        chs_set_error(error, "cmpss expects m32 source");
        return false;
    }
    if (imm->kind != CHS_X86_OP_IMM) {
        chs_set_error(error, "cmpss expects an immediate predicate operand");
        return false;
    }

    if (!chs_x86_put_u8(encoded, 0xF3u) ||
        !chs_x86_emit_rex(encoded, false, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
        !chs_x86_put_u8(encoded, 0x0Fu) ||
        !chs_x86_put_u8(encoded, 0xC2u) ||
        !chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error) ||
        !chs_x86_put_u8(encoded, (uint8_t) imm->imm)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_sse_binop(const char *mnemonic,
                                     const ChsX86Operand *dst,
                                     const ChsX86Operand *src,
                                     ChsEncodedInstruction *encoded,
                                     ChsError *error) {
    uint8_t op;
    uint8_t prefix;
    int mem_width;
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};

    if (strcmp(mnemonic, "addsd") == 0) {
        op = 0x58u;
        prefix = 0xF2u;
        mem_width = 8;
    } else if (strcmp(mnemonic, "subsd") == 0) {
        op = 0x5Cu;
        prefix = 0xF2u;
        mem_width = 8;
    } else if (strcmp(mnemonic, "mulsd") == 0) {
        op = 0x59u;
        prefix = 0xF2u;
        mem_width = 8;
    } else if (strcmp(mnemonic, "divsd") == 0) {
        op = 0x5Eu;
        prefix = 0xF2u;
        mem_width = 8;
    } else if (strcmp(mnemonic, "addss") == 0) {
        op = 0x58u;
        prefix = 0xF3u;
        mem_width = 4;
    } else if (strcmp(mnemonic, "subss") == 0) {
        op = 0x5Cu;
        prefix = 0xF3u;
        mem_width = 4;
    } else if (strcmp(mnemonic, "mulss") == 0) {
        op = 0x59u;
        prefix = 0xF3u;
        mem_width = 4;
    } else if (strcmp(mnemonic, "divss") == 0) {
        op = 0x5Eu;
        prefix = 0xF3u;
        mem_width = 4;
    } else {
        chs_set_error(error, "unsupported sse binop %s", mnemonic);
        return false;
    }

    if (dst->kind != CHS_X86_OP_REG || !dst->reg.is_xmm || (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM)) {
        chs_set_error(error, "%s expects xmm destination and xmm/m%d source", mnemonic, mem_width * 8);
        return false;
    }
    if (src->kind == CHS_X86_OP_MEM && src->mem.width != 0 && src->mem.width != mem_width) {
        chs_set_error(error, "%s expects m%d source", mnemonic, mem_width * 8);
        return false;
    }

    if (!chs_x86_put_u8(encoded, prefix) ||
        !chs_x86_emit_rex(encoded, false, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
        !chs_x86_put_u8(encoded, 0x0Fu) ||
        !chs_x86_put_u8(encoded, op) ||
        !chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_cvtsi2sd(const ChsX86Operand *dst,
                                    const ChsX86Operand *src,
                                    ChsEncodedInstruction *encoded,
                                    ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};
    bool src64;

    if (dst->kind != CHS_X86_OP_REG || !dst->reg.is_xmm || (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM)) {
        chs_set_error(error, "cvtsi2sd expects xmm destination and integer reg/mem source");
        return false;
    }
    src64 = (src->kind == CHS_X86_OP_REG) ? (src->reg.width == 64) : (src->mem.width == 8);

    if (!chs_x86_put_u8(encoded, 0xF2u) ||
        !chs_x86_emit_rex(encoded, src64, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
        !chs_x86_put_u8(encoded, 0x0Fu) ||
        !chs_x86_put_u8(encoded, 0x2Au) ||
        !chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_cvtss2sd(const ChsX86Operand *dst,
                                    const ChsX86Operand *src,
                                    ChsEncodedInstruction *encoded,
                                    ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};

    if (dst->kind != CHS_X86_OP_REG || !dst->reg.is_xmm) {
        chs_set_error(error, "cvtss2sd expects xmm destination");
        return false;
    }
    if (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM) {
        chs_set_error(error, "cvtss2sd expects xmm/m32 source");
        return false;
    }
    if (src->kind == CHS_X86_OP_MEM && src->mem.width != 0 && src->mem.width != 4) {
        chs_set_error(error, "cvtss2sd expects m32 source");
        return false;
    }

    if (!chs_x86_put_u8(encoded, 0xF3u) ||
        !chs_x86_emit_rex(encoded, false, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
        !chs_x86_put_u8(encoded, 0x0Fu) ||
        !chs_x86_put_u8(encoded, 0x5Au) ||
        !chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_cvtsi2ss(const ChsX86Operand *dst,
                                    const ChsX86Operand *src,
                                    ChsEncodedInstruction *encoded,
                                    ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};
    bool src64;

    if (dst->kind != CHS_X86_OP_REG || !dst->reg.is_xmm || (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM)) {
        chs_set_error(error, "cvtsi2ss expects xmm destination and integer reg/mem source");
        return false;
    }
    src64 = (src->kind == CHS_X86_OP_REG) ? (src->reg.width == 64) : (src->mem.width == 8);

    if (!chs_x86_put_u8(encoded, 0xF3u) ||
        !chs_x86_emit_rex(encoded, src64, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
        !chs_x86_put_u8(encoded, 0x0Fu) ||
        !chs_x86_put_u8(encoded, 0x2Au) ||
        !chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_encode_cvttsd2si(const ChsX86Operand *dst,
                                     const ChsX86Operand *src,
                                     ChsEncodedInstruction *encoded,
                                     ChsError *error) {
    uint8_t reloc_offset = 0;
    bool has_reloc = false;
    char reloc_symbol[128] = {0};
    bool dst64;

    if (dst->kind != CHS_X86_OP_REG || dst->reg.is_xmm || (src->kind != CHS_X86_OP_REG && src->kind != CHS_X86_OP_MEM)) {
        chs_set_error(error, "cvttsd2si expects integer reg destination and xmm/m64 source");
        return false;
    }
    dst64 = (dst->reg.width == 64);

    if (!chs_x86_put_u8(encoded, 0xF2u) ||
        !chs_x86_emit_rex(encoded, dst64, dst->reg.code, 0, (src->kind == CHS_X86_OP_REG) ? src->reg.code : (src->mem.has_base ? src->mem.base.code : 0)) ||
        !chs_x86_put_u8(encoded, 0x0Fu) ||
        !chs_x86_put_u8(encoded, 0x2Cu) ||
        !chs_x86_emit_rm(encoded, (uint8_t) dst->reg.code, src, &reloc_offset, &has_reloc, reloc_symbol, error)) {
        return false;
    }

    if (has_reloc) {
        encoded->uses_symbol = true;
        encoded->symbol_name = chs_strdup(reloc_symbol, error);
        if (encoded->symbol_name == NULL) {
            return false;
        }
        encoded->relocation_kind = CHS_RELOC_X86_64_SIGNED32;
        encoded->pc_relative = true;
        encoded->relocation_offset = reloc_offset;
        encoded->relocation_addend = 0;
    }

    return true;
}

static bool chs_x86_64_encode_instruction(const ChsObject *object,
                                          const ChsSection *section,
                                          uint64_t section_offset,
                                          const char *mnemonic,
                                          const char *operands,
                                          ChsEncodedInstruction *encoded,
                                          ChsError *error) {
    char opv[3][128];
    size_t op_count;
    ChsX86Operand op0;
    ChsX86Operand op1;

    (void) object;
    (void) section;
    (void) section_offset;

    memset(encoded, 0, sizeof(*encoded));
    encoded->fill_byte = 0x90u;

    op_count = chs_x86_split_operands(operands, opv);
    memset(&op0, 0, sizeof(op0));
    memset(&op1, 0, sizeof(op1));
    if (op_count >= 1 && !chs_x86_parse_operand(opv[0], &op0, error)) {
        return false;
    }
    if (op_count >= 2 && !chs_x86_parse_operand(opv[1], &op1, error)) {
        return false;
    }

    if (strcmp(mnemonic, "ret") == 0) {
        return chs_x86_put_u8(encoded, 0xC3u);
    }
    if (strcmp(mnemonic, "cqo") == 0) {
        return chs_x86_put_u8(encoded, 0x48u) && chs_x86_put_u8(encoded, 0x99u);
    }
    if (strcmp(mnemonic, "leave") == 0) {
        return chs_x86_put_u8(encoded, 0xC9u);
    }
    if (strcmp(mnemonic, "push") == 0 || strcmp(mnemonic, "pop") == 0) {
        return chs_x86_encode_push_pop(mnemonic, &op0, encoded, error);
    }
    if (strcmp(mnemonic, "call") == 0 || strcmp(mnemonic, "jmp") == 0 || mnemonic[0] == 'j') {
        return chs_x86_encode_branch_label(mnemonic, &op0, encoded, error);
    }
    if (strncmp(mnemonic, "set", 3) == 0) {
        return chs_x86_encode_setcc(mnemonic, &op0, encoded, error);
    }
    if (strcmp(mnemonic, "lea") == 0) {
        return chs_x86_encode_lea(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "mov") == 0 || strcmp(mnemonic, "movsd") == 0) {
        return chs_x86_encode_mov(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "movq") == 0) {
        return chs_x86_encode_movq(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "movd") == 0) {
        return chs_x86_encode_movd(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "cmpsd") == 0) {
        ChsX86Operand op2;
        memset(&op2, 0, sizeof(op2));
        if (op_count < 3 || !chs_x86_parse_operand(opv[2], &op2, error)) {
            chs_set_error(error, "cmpsd expects 3 operands");
            return false;
        }
        return chs_x86_encode_cmpsd(&op0, &op1, &op2, encoded, error);
    }
    if (strcmp(mnemonic, "cmpss") == 0) {
        ChsX86Operand op2;
        memset(&op2, 0, sizeof(op2));
        if (op_count < 3 || !chs_x86_parse_operand(opv[2], &op2, error)) {
            chs_set_error(error, "cmpss expects 3 operands");
            return false;
        }
        return chs_x86_encode_cmpss(&op0, &op1, &op2, encoded, error);
    }
    if (strcmp(mnemonic, "addsd") == 0 || strcmp(mnemonic, "subsd") == 0 ||
        strcmp(mnemonic, "mulsd") == 0 || strcmp(mnemonic, "divsd") == 0 ||
        strcmp(mnemonic, "addss") == 0 || strcmp(mnemonic, "subss") == 0 ||
        strcmp(mnemonic, "mulss") == 0 || strcmp(mnemonic, "divss") == 0) {
        return chs_x86_encode_sse_binop(mnemonic, &op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "cvtsi2sd") == 0) {
        return chs_x86_encode_cvtsi2sd(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "cvtsi2ss") == 0) {
        return chs_x86_encode_cvtsi2ss(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "cvtss2sd") == 0) {
        return chs_x86_encode_cvtss2sd(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "cvttsd2si") == 0) {
        return chs_x86_encode_cvttsd2si(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "movsxd") == 0 || strcmp(mnemonic, "movzx") == 0 || strcmp(mnemonic, "movsx") == 0) {
        return chs_x86_encode_movsx_family(mnemonic, &op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "imul") == 0) {
        return chs_x86_encode_imul(&op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "div") == 0) {
        return chs_x86_encode_unary_rm(mnemonic, 6, &op0, encoded, error);
    }
    if (strcmp(mnemonic, "idiv") == 0) {
        return chs_x86_encode_unary_rm(mnemonic, 7, &op0, encoded, error);
    }
    if (strcmp(mnemonic, "not") == 0) {
        return chs_x86_encode_unary_rm(mnemonic, 2, &op0, encoded, error);
    }
    if (strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "sub") == 0 || strcmp(mnemonic, "cmp") == 0 ||
        strcmp(mnemonic, "xor") == 0 || strcmp(mnemonic, "and") == 0 || strcmp(mnemonic, "or") == 0) {
        return chs_x86_encode_binop_rrm(mnemonic, &op0, &op1, encoded, error);
    }
    if (strcmp(mnemonic, "shl") == 0 || strcmp(mnemonic, "sal") == 0 || strcmp(mnemonic, "shr") == 0 || strcmp(mnemonic, "sar") == 0) {
        return chs_x86_encode_shift(mnemonic, &op0, &op1, encoded, error);
    }

    chs_set_error(error, "x86_64 encoder not implemented yet for instruction '%s'", mnemonic);
    return false;
}

const ChsArchOps chs_x86_64_arch_ops = {
    CHS_ARCH_X86_64,
    "x86_64",
    8,
    16,
    chs_x86_64_encode_instruction
};
