#include "chs/arch.h"
#include "chs/object.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool is_64_bit;
    bool is_sp;
    bool is_zero;
    unsigned index;
} ChsArm64Register;

static void chs_arm64_copy_trimmed(char *destination, size_t destination_size, const char *source) {
    size_t length;

    while (*source != '\0' && isspace((unsigned char) *source)) {
        ++source;
    }
    length = strlen(source);
    while (length > 0 && isspace((unsigned char) source[length - 1])) {
        --length;
    }
    if (length >= destination_size) {
        length = destination_size - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static size_t chs_arm64_split_operands(const char *text, char operands[4][128]) {
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
            if (depth != 0) {
                --depth;
            }
        } else if (*cursor == ',' && depth == 0) {
            if (count < 4) {
                size_t length = (size_t) (cursor - start);
                if (length >= sizeof(operands[count])) {
                    length = sizeof(operands[count]) - 1;
                }
                memcpy(operands[count], start, length);
                operands[count][length] = '\0';
                chs_arm64_copy_trimmed(operands[count], sizeof(operands[count]), operands[count]);
                ++count;
            }
            start = cursor + 1;
        }
        ++cursor;
    }

    if (*start != '\0' && count < 4) {
        size_t length = (size_t) (cursor - start);
        if (length >= sizeof(operands[count])) {
            length = sizeof(operands[count]) - 1;
        }
        memcpy(operands[count], start, length);
        operands[count][length] = '\0';
        chs_arm64_copy_trimmed(operands[count], sizeof(operands[count]), operands[count]);
        ++count;
    }

    return count;
}

static bool chs_arm64_parse_register(const char *text, ChsArm64Register *result) {
    char name[32];
    uint64_t parsed_number;

    chs_arm64_copy_trimmed(name, sizeof(name), text);
    if (strcmp(name, "sp") == 0) {
        result->is_64_bit = true;
        result->is_sp = true;
        result->is_zero = false;
        result->index = 31;
        return true;
    }
    if (strcmp(name, "xzr") == 0) {
        result->is_64_bit = true;
        result->is_sp = false;
        result->is_zero = true;
        result->index = 31;
        return true;
    }
    if (strcmp(name, "wzr") == 0) {
        result->is_64_bit = false;
        result->is_sp = false;
        result->is_zero = true;
        result->index = 31;
        return true;
    }
    if ((name[0] == 'x' || name[0] == 'w') &&
        chs_parse_u64(name + 1, &parsed_number) &&
        parsed_number <= 30u) {
        result->is_64_bit = name[0] == 'x';
        result->is_sp = false;
        result->is_zero = false;
        result->index = (unsigned) parsed_number;
        return true;
    }
    return false;
}

static bool chs_arm64_parse_d_register(const char *text, unsigned *index) {
    char name[32];
    uint64_t parsed_number;

    chs_arm64_copy_trimmed(name, sizeof(name), text);
    if (name[0] != 'd') {
        return false;
    }
    if (!chs_parse_u64(name + 1, &parsed_number) || parsed_number > 31u) {
        return false;
    }
    *index = (unsigned) parsed_number;
    return true;
}

static bool chs_arm64_parse_s_register(const char *text, unsigned *index) {
    char name[32];
    uint64_t parsed_number;

    chs_arm64_copy_trimmed(name, sizeof(name), text);
    if (name[0] != 's') {
        return false;
    }
    if (!chs_parse_u64(name + 1, &parsed_number) || parsed_number > 31u) {
        return false;
    }
    *index = (unsigned) parsed_number;
    return true;
}

static bool chs_arm64_parse_immediate(const char *text, int64_t *value) {
    char copy[128];

    chs_arm64_copy_trimmed(copy, sizeof(copy), text);
    if (copy[0] == '#') {
        memmove(copy, copy + 1, strlen(copy));
    }
    return chs_parse_i64(copy, value);
}

static bool chs_arm64_parse_memory_operand(const char *text,
                                           ChsArm64Register *base,
                                           int64_t *immediate,
                                           bool *pre_index,
                                           bool *post_index) {
    char copy[128];
    char *closing_bracket;
    char *comma;
    char *post_text;

    chs_arm64_copy_trimmed(copy, sizeof(copy), text);
    *pre_index = false;
    *post_index = false;
    *immediate = 0;

    if (copy[0] != '[') {
        return false;
    }

    closing_bracket = strchr(copy, ']');
    if (closing_bracket == NULL) {
        return false;
    }
    *closing_bracket = '\0';

    comma = strchr(copy + 1, ',');
    if (comma != NULL) {
        *comma = '\0';
        ++comma;
        if (!chs_arm64_parse_immediate(comma, immediate)) {
            return false;
        }
    }

    if (!chs_arm64_parse_register(copy + 1, base)) {
        return false;
    }

    post_text = chs_trim(closing_bracket + 1);
    if (*post_text == '!') {
        *pre_index = true;
        return true;
    }
    if (*post_text == ',') {
        *post_index = true;
        return chs_arm64_parse_immediate(post_text + 1, immediate);
    }
    return *post_text == '\0';
}

static unsigned chs_arm64_condition_code(const char *text) {
    if (strcmp(text, "eq") == 0) {
        return 0;
    }
    if (strcmp(text, "ne") == 0) {
        return 1;
    }
    if (strcmp(text, "cs") == 0 || strcmp(text, "hs") == 0) {
        return 2;
    }
    if (strcmp(text, "cc") == 0 || strcmp(text, "lo") == 0) {
        return 3;
    }
    if (strcmp(text, "mi") == 0) {
        return 4;
    }
    if (strcmp(text, "pl") == 0) {
        return 5;
    }
    if (strcmp(text, "vs") == 0) {
        return 6;
    }
    if (strcmp(text, "vc") == 0) {
        return 7;
    }
    if (strcmp(text, "hi") == 0) {
        return 8;
    }
    if (strcmp(text, "ls") == 0) {
        return 9;
    }
    if (strcmp(text, "ge") == 0) {
        return 10;
    }
    if (strcmp(text, "lt") == 0) {
        return 11;
    }
    if (strcmp(text, "gt") == 0) {
        return 12;
    }
    if (strcmp(text, "le") == 0) {
        return 13;
    }
    if (strcmp(text, "al") == 0) {
        return 14;
    }
    return 0xffu;
}

static uint32_t chs_arm64_encode_add_sub_immediate(bool is_sub, bool is_64_bit, unsigned rd, unsigned rn, uint32_t imm12) {
    uint32_t base;

    base = is_64_bit ? (is_sub ? 0xD1000000u : 0x91000000u) : (is_sub ? 0x51000000u : 0x11000000u);
    return base | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_add_sub_register(bool is_sub,
                                                  bool is_64_bit,
                                                  unsigned rd,
                                                  unsigned rn,
                                                  unsigned rm) {
    uint32_t base;

    base = is_64_bit ? (is_sub ? 0xCB000000u : 0x8B000000u)
                     : (is_sub ? 0x4B000000u : 0x0B000000u);
    return base | ((uint32_t) rm << 16) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_orr_alias(bool is_64_bit, unsigned rd, unsigned rm) {
    return (is_64_bit ? 0xAA0003E0u : 0x2A0003E0u) | ((uint32_t) rm << 16) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_mvn_alias(bool is_64_bit, unsigned rd, unsigned rm) {
    return (is_64_bit ? 0xAA2003E0u : 0x2A2003E0u) | ((uint32_t) rm << 16) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_logical_register(const char *mnemonic,
                                                  bool is_64_bit,
                                                  unsigned rd,
                                                  unsigned rn,
                                                  unsigned rm) {
    uint32_t base;

    if (strcmp(mnemonic, "and") == 0) {
        base = is_64_bit ? 0x8A000000u : 0x0A000000u;
    } else if (strcmp(mnemonic, "orr") == 0) {
        base = is_64_bit ? 0xAA000000u : 0x2A000000u;
    } else {
        base = is_64_bit ? 0xCA000000u : 0x4A000000u;
    }
    return base | ((uint32_t) rm << 16) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_movz(bool is_64_bit,
                                      unsigned rd,
                                      uint32_t imm16,
                                      unsigned shift_bits) {
    uint32_t base;
    uint32_t hw;

    base = is_64_bit ? 0xD2800000u : 0x52800000u;
    hw = (shift_bits / 16u) & 0x3u;
    return base | (hw << 21) | ((imm16 & 0xffffu) << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_movk(bool is_64_bit,
                                      unsigned rd,
                                      uint32_t imm16,
                                      unsigned shift_bits) {
    uint32_t base;
    uint32_t hw;

    base = is_64_bit ? 0xF2800000u : 0x72800000u;
    hw = (shift_bits / 16u) & 0x3u;
    return base | (hw << 21) | ((imm16 & 0xffffu) << 5) | (uint32_t) rd;
}

static bool chs_arm64_parse_lsl_shift(const char *text, unsigned *shift_bits) {
    char copy[64];
    char *operand;
    int64_t value;

    chs_arm64_copy_trimmed(copy, sizeof(copy), text);
    operand = copy;
    if (strncmp(operand, "lsl", 3) == 0) {
        operand = chs_trim(operand + 3);
    }
    if (!chs_arm64_parse_immediate(operand, &value) || value < 0) {
        return false;
    }
    *shift_bits = (unsigned) value;
    return true;
}

static uint32_t chs_arm64_encode_load_store_unsigned(bool is_load, unsigned element_bits, unsigned rt, unsigned rn, uint32_t imm12) {
    uint32_t base;

    if (element_bits == 8) {
        base = is_load ? 0x39400000u : 0x39000000u;
    } else if (element_bits == 16) {
        base = is_load ? 0x79400000u : 0x79000000u;
    } else if (element_bits == 32) {
        base = is_load ? 0xB9400000u : 0xB9000000u;
    } else {
        base = is_load ? 0xF9400000u : 0xF9000000u;
    }
    return base | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

static uint32_t chs_arm64_encode_fp64_load_store_unsigned(bool is_load,
                                                           unsigned rt,
                                                           unsigned rn,
                                                           uint32_t imm12) {
    uint32_t base;

    base = is_load ? 0xFD400000u : 0xFD000000u;
    return base | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

static uint32_t chs_arm64_encode_fp32_load_store_unsigned(bool is_load,
                                                           unsigned rt,
                                                           unsigned rn,
                                                           uint32_t imm12) {
    uint32_t base;

    base = is_load ? 0xBD400000u : 0xBD000000u;
    return base | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

static uint32_t chs_arm64_encode_ldrsb_unsigned(bool destination_is_64,
                                                unsigned rt,
                                                unsigned rn,
                                                uint32_t imm12) {
    uint32_t base;

    base = destination_is_64 ? 0x39C00000u : 0x39800000u;
    return base | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

static uint32_t chs_arm64_encode_ldrsh_unsigned(bool destination_is_64,
                                                unsigned rt,
                                                unsigned rn,
                                                uint32_t imm12) {
    uint32_t base;

    base = destination_is_64 ? 0x79C00000u : 0x79800000u;
    return base | (imm12 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

static uint32_t chs_arm64_encode_stp_ldp(bool is_load,
                                         bool post_index,
                                         unsigned rt,
                                         unsigned rt2,
                                         unsigned rn,
                                         int64_t immediate) {
    uint32_t base;
    uint32_t imm7;

    imm7 = (uint32_t) ((immediate / 8) & 0x7f);
    if (is_load) {
        base = post_index ? 0xA8C00000u : 0xA9400000u;
    } else {
        base = post_index ? 0xA8800000u : 0xA9800000u;
    }
    return base | (imm7 << 15) | ((uint32_t) rt2 << 10) | ((uint32_t) rn << 5) | (uint32_t) rt;
}

static uint32_t chs_arm64_encode_branch_imm(bool link, int64_t delta) {
    uint32_t imm26;

    imm26 = (uint32_t) ((delta >> 2) & 0x03ffffff);
    return (link ? 0x94000000u : 0x14000000u) | imm26;
}

static uint32_t chs_arm64_encode_branch_register(bool link, unsigned rn) {
    return (link ? 0xD63F0000u : 0xD61F0000u) | ((uint32_t) rn << 5);
}

static uint32_t chs_arm64_encode_mul(bool is_64_bit,
                                     unsigned rd,
                                     unsigned rn,
                                     unsigned rm) {
    uint32_t base;

    base = is_64_bit ? 0x9B007C00u : 0x1B007C00u;
    return base | ((uint32_t) rm << 16) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_div(bool is_signed,
                                     bool is_64_bit,
                                     unsigned rd,
                                     unsigned rn,
                                     unsigned rm) {
    uint32_t base;

    if (is_64_bit) {
        base = is_signed ? 0x9AC00C00u : 0x9AC00800u;
    } else {
        base = is_signed ? 0x1AC00C00u : 0x1AC00800u;
    }
    return base | ((uint32_t) rm << 16) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_madd_msub(bool is_sub,
                                           bool is_64_bit,
                                           unsigned rd,
                                           unsigned rn,
                                           unsigned rm,
                                           unsigned ra) {
    uint32_t base;

    if (is_64_bit) {
        base = is_sub ? 0x9B008000u : 0x9B000000u;
    } else {
        base = is_sub ? 0x1B008000u : 0x1B000000u;
    }
    return base | ((uint32_t) rm << 16) | ((uint32_t) ra << 10) |
           ((uint32_t) rn << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_shift_variable(const char *mnemonic,
                                                bool is_64_bit,
                                                unsigned rd,
                                                unsigned rn,
                                                unsigned rm) {
    uint32_t base;

    if (strcmp(mnemonic, "lsl") == 0) {
        base = is_64_bit ? 0x9AC02000u : 0x1AC02000u;
    } else if (strcmp(mnemonic, "lsr") == 0) {
        base = is_64_bit ? 0x9AC02400u : 0x1AC02400u;
    } else {
        base = is_64_bit ? 0x9AC02800u : 0x1AC02800u;
    }
    return base | ((uint32_t) rm << 16) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_fmov_d_from_x(unsigned dd, unsigned xn) {
    return 0x9E670000u | ((uint32_t) xn << 5) | (uint32_t) dd;
}

static uint32_t chs_arm64_encode_fmov_x_from_d(unsigned xd, unsigned dn) {
    return 0x9E660000u | ((uint32_t) dn << 5) | (uint32_t) xd;
}

static uint32_t chs_arm64_encode_fmov_s_from_w(unsigned sd, unsigned wn) {
    return 0x1E270000u | ((uint32_t) wn << 5) | (uint32_t) sd;
}

static uint32_t chs_arm64_encode_fmov_w_from_s(unsigned wd, unsigned sn) {
    return 0x1E260000u | ((uint32_t) sn << 5) | (uint32_t) wd;
}

static uint32_t chs_arm64_encode_fcmp(bool compare_zero,
                                      bool is_double,
                                      unsigned dn,
                                      unsigned dm) {
    if (compare_zero) {
        return (is_double ? 0x1E602008u : 0x1E202008u) | ((uint32_t) dn << 5);
    }
    return (is_double ? 0x1E602000u : 0x1E202000u) |
           ((uint32_t) dm << 16) | ((uint32_t) dn << 5);
}

static uint32_t chs_arm64_encode_fp_arith(const char *mnemonic,
                                          bool is_double,
                                          unsigned dd,
                                          unsigned dn,
                                          unsigned dm) {
    uint32_t base;

    if (strcmp(mnemonic, "fadd") == 0) {
        base = is_double ? 0x1E602800u : 0x1E202800u;
    } else if (strcmp(mnemonic, "fsub") == 0) {
        base = is_double ? 0x1E603800u : 0x1E203800u;
    } else if (strcmp(mnemonic, "fmul") == 0) {
        base = is_double ? 0x1E600800u : 0x1E200800u;
    } else {
        base = is_double ? 0x1E601800u : 0x1E201800u;
    }
    return base | ((uint32_t) dm << 16) | ((uint32_t) dn << 5) | (uint32_t) dd;
}

static uint32_t chs_arm64_encode_fcvt_to_int(bool unsigned_result,
                                             bool dest_is_64,
                                             unsigned rd,
                                             unsigned dn) {
    uint32_t base;

    if (unsigned_result) {
        base = dest_is_64 ? 0x9E790000u : 0x1E790000u;
    } else {
        base = dest_is_64 ? 0x9E780000u : 0x1E780000u;
    }
    return base | ((uint32_t) dn << 5) | (uint32_t) rd;
}

static uint32_t chs_arm64_encode_int_to_fcvt(bool unsigned_source,
                                             bool source_is_64,
                                             bool destination_is_double,
                                             unsigned dd,
                                             unsigned rn) {
    uint32_t base;

    if (unsigned_source) {
        if (source_is_64) {
            base = destination_is_double ? 0x9E630000u : 0x9E230000u;
        } else {
            base = destination_is_double ? 0x1E630000u : 0x1E230000u;
        }
    } else {
        if (source_is_64) {
            base = destination_is_double ? 0x9E620000u : 0x9E220000u;
        } else {
            base = destination_is_double ? 0x1E620000u : 0x1E220000u;
        }
    }
    return base | ((uint32_t) rn << 5) | (uint32_t) dd;
}

static uint32_t chs_arm64_encode_fcvt_scalar(bool to_double,
                                             unsigned vd,
                                             unsigned vn) {
    uint32_t base;

    base = to_double ? 0x1E22C000u : 0x1E624000u;
    return base | ((uint32_t) vn << 5) | (uint32_t) vd;
}

static uint32_t chs_arm64_encode_cond_branch(unsigned condition, int64_t delta) {
    uint32_t imm19;

    imm19 = (uint32_t) ((delta >> 2) & 0x7ffff);
    return 0x54000000u | (imm19 << 5) | condition;
}

static uint32_t chs_arm64_encode_cmp_register(bool is_64_bit, unsigned rn, unsigned rm) {
    return (is_64_bit ? 0xEB00001Fu : 0x6B00001Fu) | ((uint32_t) rm << 16) | ((uint32_t) rn << 5);
}

static uint32_t chs_arm64_encode_cset(unsigned rd, unsigned condition) {
    return 0x1A800400u | ((uint32_t) 31 << 16) | (((condition ^ 1u) & 0xfu) << 12) | ((uint32_t) 31 << 5) | rd;
}

static uint32_t chs_arm64_encode_extend(bool sign_extend, unsigned rd, unsigned rn) {
    return (sign_extend ? 0x93407C00u : 0xD3407C00u) | ((uint32_t) rn << 5) | rd;
}

static uint32_t chs_arm64_encode_bitfield_extract(bool sign_extend,
                                                  bool is_64_bit,
                                                  unsigned rd,
                                                  unsigned rn,
                                                  unsigned imms) {
    uint32_t base;

    base = is_64_bit ? (sign_extend ? 0x93400000u : 0xD3400000u)
                     : (sign_extend ? 0x13000000u : 0x53000000u);
    return base | ((imms & 0x3fu) << 10) | ((uint32_t) rn << 5) | (uint32_t) rd;
}

static bool chs_arm64_find_defined_symbol(const ChsObject *object,
                                          const ChsSection *section,
                                          const char *name,
                                          uint64_t *value) {
    size_t symbol_index;
    const ChsSymbol *symbol;

    symbol = chs_object_find_symbol(object, name, &symbol_index);
    if (symbol == NULL || !symbol->defined || symbol->section_index >= object->section_count) {
        return false;
    }
    if (&object->sections[symbol->section_index] != section) {
        return false;
    }
    *value = symbol->value;
    return true;
}

static bool chs_arm64_encode_instruction(const ChsObject *object,
                                         const ChsSection *section,
                                         uint64_t section_offset,
                                         const char *mnemonic,
                                         const char *operands_text,
                                         ChsEncodedInstruction *encoded,
                                         ChsError *error) {
    char operands[4][128];
    size_t operand_count;

    memset(encoded, 0, sizeof(*encoded));
    encoded->mnemonic = mnemonic;
    encoded->operands = operands_text;
    operand_count = chs_arm64_split_operands(operands_text, operands);

    if (strcmp(mnemonic, "ret") == 0 && (operand_count == 0 || operand_count == 1)) {
        if (operand_count == 1) {
            ChsArm64Register source;

            if (!chs_arm64_parse_register(operands[0], &source) || !source.is_64_bit || source.is_sp || source.is_zero) {
                chs_set_error(error, "invalid ret operand: %s", operands[0]);
                return false;
            }
            encoded->encoded = 0xD65F0000u | ((uint32_t) source.index << 5);
            return true;
        }
        encoded->encoded = 0xD65F03C0u;
        return true;
    }

    if ((strcmp(mnemonic, "br") == 0 || strcmp(mnemonic, "blr") == 0) && operand_count == 1) {
        ChsArm64Register target;

        if (!chs_arm64_parse_register(operands[0], &target) || !target.is_64_bit || target.is_sp || target.is_zero) {
            chs_set_error(error, "invalid %s operand: %s", mnemonic, operands[0]);
            return false;
        }
        encoded->encoded = chs_arm64_encode_branch_register(strcmp(mnemonic, "blr") == 0,
                                                            target.index);
        return true;
    }

    if (strcmp(mnemonic, "mov") == 0 && operand_count == 2) {
        ChsArm64Register destination;
        ChsArm64Register source;
        bool destination_64;
        bool source_64;

        if (!chs_arm64_parse_register(operands[0], &destination) || !chs_arm64_parse_register(operands[1], &source)) {
            chs_set_error(error, "invalid mov operands: %s", operands_text);
            return false;
        }
        if (destination.is_sp || source.is_sp) {
            encoded->encoded = chs_arm64_encode_add_sub_immediate(false, true, destination.index, source.index, 0);
            return true;
        }
        destination_64 = destination.is_64_bit || (destination.is_zero && source.is_64_bit);
        source_64 = source.is_64_bit || (source.is_zero && destination.is_64_bit);
        if (destination_64 != source_64) {
            chs_set_error(error, "mov register width mismatch: %s", operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_orr_alias(destination_64, destination.index, source.index);
        return true;
    }

    if (strcmp(mnemonic, "mvn") == 0 && operand_count == 2) {
        ChsArm64Register destination;
        ChsArm64Register source;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_register(operands[1], &source)) {
            chs_set_error(error, "invalid mvn operands: %s", operands_text);
            return false;
        }
        if (destination.is_sp || source.is_sp || destination.is_64_bit != source.is_64_bit) {
            chs_set_error(error, "mvn register width mismatch: %s", operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_mvn_alias(destination.is_64_bit,
                                                      destination.index,
                                                      source.index);
        return true;
    }

    if (strcmp(mnemonic, "fmov") == 0 && operand_count == 2) {
        unsigned dd;
        unsigned dn;
        unsigned sd;
        unsigned sn;
        ChsArm64Register xr;

        if (chs_arm64_parse_d_register(operands[0], &dd) &&
            chs_arm64_parse_register(operands[1], &xr) && xr.is_64_bit && !xr.is_sp) {
            encoded->encoded = chs_arm64_encode_fmov_d_from_x(dd, xr.index);
            return true;
        }
        if (chs_arm64_parse_register(operands[0], &xr) && xr.is_64_bit && !xr.is_sp &&
            chs_arm64_parse_d_register(operands[1], &dn)) {
            encoded->encoded = chs_arm64_encode_fmov_x_from_d(xr.index, dn);
            return true;
        }
        if (chs_arm64_parse_s_register(operands[0], &sd) &&
            chs_arm64_parse_register(operands[1], &xr) && !xr.is_64_bit && !xr.is_sp) {
            encoded->encoded = chs_arm64_encode_fmov_s_from_w(sd, xr.index);
            return true;
        }
        if (chs_arm64_parse_register(operands[0], &xr) && !xr.is_64_bit && !xr.is_sp &&
            chs_arm64_parse_s_register(operands[1], &sn)) {
            encoded->encoded = chs_arm64_encode_fmov_w_from_s(xr.index, sn);
            return true;
        }
        chs_set_error(error, "invalid fmov operands: %s", operands_text);
        return false;
    }

    if (strcmp(mnemonic, "fcmp") == 0 && operand_count == 2) {
        bool is_double;
        unsigned dn;
        unsigned dm;
        int64_t immediate;

        if (chs_arm64_parse_d_register(operands[0], &dn)) {
            is_double = true;
        } else if (chs_arm64_parse_s_register(operands[0], &dn)) {
            is_double = false;
        } else {
            chs_set_error(error, "invalid fcmp operands: %s", operands_text);
            return false;
        }

        if ((is_double && chs_arm64_parse_d_register(operands[1], &dm)) ||
            (!is_double && chs_arm64_parse_s_register(operands[1], &dm))) {
            encoded->encoded = chs_arm64_encode_fcmp(false, is_double, dn, dm);
            return true;
        }
        if (chs_arm64_parse_immediate(operands[1], &immediate) && immediate == 0) {
            encoded->encoded = chs_arm64_encode_fcmp(true, is_double, dn, 0);
            return true;
        }
        chs_set_error(error, "invalid fcmp operands: %s", operands_text);
        return false;
    }

    if ((strcmp(mnemonic, "fadd") == 0 || strcmp(mnemonic, "fsub") == 0 ||
         strcmp(mnemonic, "fmul") == 0 || strcmp(mnemonic, "fdiv") == 0) &&
        operand_count == 3) {
        bool is_double;
        unsigned dd;
        unsigned dn;
        unsigned dm;

        if (chs_arm64_parse_d_register(operands[0], &dd)) {
            is_double = true;
        } else if (chs_arm64_parse_s_register(operands[0], &dd)) {
            is_double = false;
        } else {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        if ((is_double && (!chs_arm64_parse_d_register(operands[1], &dn) ||
                           !chs_arm64_parse_d_register(operands[2], &dm))) ||
            (!is_double && (!chs_arm64_parse_s_register(operands[1], &dn) ||
                            !chs_arm64_parse_s_register(operands[2], &dm)))) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_fp_arith(mnemonic, is_double, dd, dn, dm);
        return true;
    }

    if ((strcmp(mnemonic, "fcvtzu") == 0 || strcmp(mnemonic, "fcvtzs") == 0) &&
        operand_count == 2) {
        ChsArm64Register destination;
        unsigned dn;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            destination.is_sp || !chs_arm64_parse_d_register(operands[1], &dn)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_fcvt_to_int(strcmp(mnemonic, "fcvtzu") == 0,
                                                        destination.is_64_bit,
                                                        destination.index,
                                                        dn);
        return true;
    }

    if ((strcmp(mnemonic, "ucvtf") == 0 || strcmp(mnemonic, "scvtf") == 0) &&
        operand_count == 2) {
        unsigned dd;
        bool destination_is_double;
        ChsArm64Register source;

        if (chs_arm64_parse_d_register(operands[0], &dd)) {
            destination_is_double = true;
        } else if (chs_arm64_parse_s_register(operands[0], &dd)) {
            destination_is_double = false;
        } else {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }

        if (
            !chs_arm64_parse_register(operands[1], &source) || source.is_sp) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_int_to_fcvt(strcmp(mnemonic, "ucvtf") == 0,
                                                        source.is_64_bit,
                                                        destination_is_double,
                                                        dd,
                                                        source.index);
        return true;
    }

    if (strcmp(mnemonic, "fcvt") == 0 && operand_count == 2) {
        unsigned vd;
        unsigned vn;

        if (chs_arm64_parse_d_register(operands[0], &vd) &&
            chs_arm64_parse_s_register(operands[1], &vn)) {
            encoded->encoded = chs_arm64_encode_fcvt_scalar(true, vd, vn);
            return true;
        }
        if (chs_arm64_parse_s_register(operands[0], &vd) &&
            chs_arm64_parse_d_register(operands[1], &vn)) {
            encoded->encoded = chs_arm64_encode_fcvt_scalar(false, vd, vn);
            return true;
        }
        chs_set_error(error, "invalid fcvt operands: %s", operands_text);
        return false;
    }

    if (strcmp(mnemonic, "movz") == 0 && (operand_count == 2 || operand_count == 3)) {
        ChsArm64Register destination;
        int64_t immediate;
        unsigned shift_bits;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_immediate(operands[1], &immediate)) {
            chs_set_error(error, "invalid movz operands: %s", operands_text);
            return false;
        }

        shift_bits = 0;
        if (operand_count == 3) {
            if (!chs_arm64_parse_lsl_shift(operands[2], &shift_bits)) {
                chs_set_error(error, "invalid movz shift: %s", operands[2]);
                return false;
            }
        }
        if ((shift_bits % 16u) != 0u || shift_bits > (destination.is_64_bit ? 48u : 16u)) {
            chs_set_error(error, "invalid movz shift amount: %u", shift_bits);
            return false;
        }

        encoded->encoded = chs_arm64_encode_movz(destination.is_64_bit,
                                                 destination.index,
                                                 (uint32_t) immediate,
                                                 shift_bits);
        return true;
    }

    if (strcmp(mnemonic, "movk") == 0 && (operand_count == 2 || operand_count == 3)) {
        ChsArm64Register destination;
        int64_t immediate;
        unsigned shift_bits;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_immediate(operands[1], &immediate)) {
            chs_set_error(error, "invalid movk operands: %s", operands_text);
            return false;
        }

        shift_bits = 0;
        if (operand_count == 3) {
            if (!chs_arm64_parse_lsl_shift(operands[2], &shift_bits)) {
                chs_set_error(error, "invalid movk shift: %s", operands[2]);
                return false;
            }
        }
        if ((shift_bits % 16u) != 0u || shift_bits > (destination.is_64_bit ? 48u : 16u)) {
            chs_set_error(error, "invalid movk shift amount: %u", shift_bits);
            return false;
        }

        encoded->encoded = chs_arm64_encode_movk(destination.is_64_bit,
                                                 destination.index,
                                                 (uint32_t) immediate,
                                                 shift_bits);
        return true;
    }

    if ((strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "sub") == 0) && operand_count == 3) {
        ChsArm64Register destination;
        ChsArm64Register left;
        ChsArm64Register right;
        int64_t immediate;

        if (!chs_arm64_parse_register(operands[0], &destination) || !chs_arm64_parse_register(operands[1], &left)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }

        if (strstr(operands[2], "@PAGEOFF") != NULL) {
            const char *at_pageoff;
            char symbol_name[128];

            at_pageoff = strstr(operands[2], "@PAGEOFF");
            memcpy(symbol_name, operands[2], (size_t) (at_pageoff - operands[2]));
            symbol_name[at_pageoff - operands[2]] = '\0';
            chs_arm64_copy_trimmed(symbol_name, sizeof(symbol_name), symbol_name);
            encoded->encoded = chs_arm64_encode_add_sub_immediate(false, true, destination.index, left.index, 0);
            encoded->uses_symbol = true;
            encoded->symbol_name = strdup(symbol_name);
            encoded->relocation_kind = CHS_RELOC_AARCH64_PAGEOFF12;
            encoded->pc_relative = false;
            return true;
        }

        if (!chs_arm64_parse_immediate(operands[2], &immediate)) {
            if (!chs_arm64_parse_register(operands[2], &right)) {
                chs_set_error(error, "invalid %s immediate: %s", mnemonic, operands[2]);
                return false;
            }
            if (destination.is_64_bit != left.is_64_bit ||
                destination.is_64_bit != right.is_64_bit) {
                chs_set_error(error, "%s register width mismatch: %s", mnemonic, operands_text);
                return false;
            }
            encoded->encoded = chs_arm64_encode_add_sub_register(strcmp(mnemonic, "sub") == 0,
                                                                 destination.is_64_bit,
                                                                 destination.index,
                                                                 left.index,
                                                                 right.index);
            return true;
        }

        encoded->encoded = chs_arm64_encode_add_sub_immediate(strcmp(mnemonic, "sub") == 0,
                                                              destination.is_64_bit || destination.is_sp,
                                                              destination.index,
                                                              left.index,
                                                              (uint32_t) immediate & 0xfffu);
        return true;
    }

    if ((strcmp(mnemonic, "and") == 0 || strcmp(mnemonic, "orr") == 0 ||
         strcmp(mnemonic, "eor") == 0) && operand_count == 3) {
        ChsArm64Register destination;
        ChsArm64Register left;
        ChsArm64Register right;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_register(operands[1], &left) ||
            !chs_arm64_parse_register(operands[2], &right)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        if (destination.is_64_bit != left.is_64_bit ||
            destination.is_64_bit != right.is_64_bit) {
            chs_set_error(error, "%s register width mismatch: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_logical_register(mnemonic,
                                                             destination.is_64_bit,
                                                             destination.index,
                                                             left.index,
                                                             right.index);
        return true;
    }

        if ((strcmp(mnemonic, "ldr") == 0 || strcmp(mnemonic, "str") == 0 ||
                strcmp(mnemonic, "ldrb") == 0 || strcmp(mnemonic, "strb") == 0 ||
                strcmp(mnemonic, "ldrh") == 0 || strcmp(mnemonic, "strh") == 0 ||
            strcmp(mnemonic, "ldrsb") == 0 || strcmp(mnemonic, "ldrsh") == 0) && operand_count == 2) {
        ChsArm64Register target;
        ChsArm64Register base;
            unsigned d_target;
            unsigned s_target;
            bool target_is_d;
            bool target_is_s;
        int64_t immediate;
        bool pre_index;
        bool post_index;
        unsigned element_bits;

            target_is_d = chs_arm64_parse_d_register(operands[0], &d_target);
            target_is_s = !target_is_d && chs_arm64_parse_s_register(operands[0], &s_target);
            if ((!target_is_d && !target_is_s && !chs_arm64_parse_register(operands[0], &target)) ||
            !chs_arm64_parse_memory_operand(operands[1], &base, &immediate, &pre_index, &post_index)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        if (pre_index || post_index) {
            chs_set_error(error, "%s pre/post-index form not implemented", mnemonic);
            return false;
        }

            if (target_is_d) {
                if (strcmp(mnemonic, "ldr") != 0 && strcmp(mnemonic, "str") != 0) {
                    chs_set_error(error, "invalid %s destination register: %s", mnemonic, operands[0]);
                    return false;
                }
                immediate /= 8;
                encoded->encoded = chs_arm64_encode_fp64_load_store_unsigned(mnemonic[0] == 'l',
                                                                              d_target,
                                                                              base.index,
                                                                              (uint32_t) immediate & 0xfffu);
                return true;
            }

            if (target_is_s) {
                if (strcmp(mnemonic, "ldr") != 0 && strcmp(mnemonic, "str") != 0) {
                    chs_set_error(error, "invalid %s destination register: %s", mnemonic, operands[0]);
                    return false;
                }
                immediate /= 4;
                encoded->encoded = chs_arm64_encode_fp32_load_store_unsigned(mnemonic[0] == 'l',
                                                                              s_target,
                                                                              base.index,
                                                                              (uint32_t) immediate & 0xfffu);
                return true;
            }

        if (strcmp(mnemonic, "ldrsb") == 0) {
            encoded->encoded = chs_arm64_encode_ldrsb_unsigned(target.is_64_bit,
                                                                target.index,
                                                                base.index,
                                                                (uint32_t) immediate & 0xfffu);
            return true;
        }

        if (strcmp(mnemonic, "ldrsh") == 0) {
            immediate /= 2;
            encoded->encoded = chs_arm64_encode_ldrsh_unsigned(target.is_64_bit,
                                                                target.index,
                                                                base.index,
                                                                (uint32_t) immediate & 0xfffu);
            return true;
        }

        if (strcmp(mnemonic, "ldrb") == 0 || strcmp(mnemonic, "strb") == 0) {
            element_bits = 8;
        } else if (strcmp(mnemonic, "ldrh") == 0 || strcmp(mnemonic, "strh") == 0) {
            element_bits = 16;
            immediate /= 2;
        } else if (target.is_64_bit) {
            element_bits = 64;
            immediate /= 8;
        } else {
            element_bits = 32;
            immediate /= 4;
        }

        encoded->encoded = chs_arm64_encode_load_store_unsigned(mnemonic[0] == 'l',
                                                                element_bits,
                                                                target.index,
                                                                base.index,
                                                                (uint32_t) immediate & 0xfffu);
        return true;
    }

    if ((strcmp(mnemonic, "stp") == 0 || strcmp(mnemonic, "ldp") == 0) && operand_count == 3) {
        ChsArm64Register first;
        ChsArm64Register second;
        ChsArm64Register base;
        int64_t immediate;
        bool pre_index;
        bool post_index;

        if (!chs_arm64_parse_register(operands[0], &first) || !chs_arm64_parse_register(operands[1], &second) ||
            !chs_arm64_parse_memory_operand(operands[2], &base, &immediate, &pre_index, &post_index)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        if (!pre_index && !post_index) {
            chs_set_error(error, "%s signed-offset form not implemented", mnemonic);
            return false;
        }
        encoded->encoded = chs_arm64_encode_stp_ldp(mnemonic[0] == 'l', post_index, first.index, second.index, base.index, immediate);
        return true;
    }

    if ((strcmp(mnemonic, "stp") == 0 || strcmp(mnemonic, "ldp") == 0) && operand_count == 4) {
        char combined_memory[256];
        ChsArm64Register first;
        ChsArm64Register second;
        ChsArm64Register base;
        int64_t immediate;
        bool pre_index;
        bool post_index;

        snprintf(combined_memory, sizeof(combined_memory), "%s, %s", operands[2], operands[3]);
        if (!chs_arm64_parse_register(operands[0], &first) || !chs_arm64_parse_register(operands[1], &second) ||
            !chs_arm64_parse_memory_operand(combined_memory, &base, &immediate, &pre_index, &post_index)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_stp_ldp(mnemonic[0] == 'l', post_index, first.index, second.index, base.index, immediate);
        return true;
    }

    if ((strcmp(mnemonic, "b") == 0 || strcmp(mnemonic, "bl") == 0) && operand_count == 1) {
        uint64_t target_value;

        if (chs_arm64_find_defined_symbol(object, section, operands[0], &target_value)) {
            int64_t delta = (int64_t) target_value - (int64_t) section_offset;
            encoded->encoded = chs_arm64_encode_branch_imm(strcmp(mnemonic, "bl") == 0, delta);
            return true;
        }
        encoded->encoded = strcmp(mnemonic, "bl") == 0 ? 0x94000000u : 0x14000000u;
        encoded->uses_symbol = true;
        encoded->symbol_name = strdup(operands[0]);
        encoded->relocation_kind = CHS_RELOC_AARCH64_BRANCH26;
        encoded->pc_relative = true;
        return true;
    }

    if (strncmp(mnemonic, "b.", 2) == 0 && operand_count == 1) {
        unsigned condition;
        uint64_t target_value;

        condition = chs_arm64_condition_code(mnemonic + 2);
        if (condition == 0xffu) {
            chs_set_error(error, "unsupported branch condition: %s", mnemonic + 2);
            return false;
        }
        if (!chs_arm64_find_defined_symbol(object, section, operands[0], &target_value)) {
            chs_set_error(error, "conditional branch requires an in-section label: %s", operands[0]);
            return false;
        }
        encoded->encoded = chs_arm64_encode_cond_branch(condition, (int64_t) target_value - (int64_t) section_offset);
        return true;
    }

    if (strcmp(mnemonic, "cmp") == 0 && operand_count == 2) {
        ChsArm64Register left;
        ChsArm64Register right;

        if (!chs_arm64_parse_register(operands[0], &left) || !chs_arm64_parse_register(operands[1], &right)) {
            chs_set_error(error, "invalid cmp operands: %s", operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_cmp_register(left.is_64_bit, left.index, right.index);
        return true;
    }

    if (strcmp(mnemonic, "cset") == 0 && operand_count == 2) {
        ChsArm64Register destination;
        unsigned condition;

        if (!chs_arm64_parse_register(operands[0], &destination)) {
            chs_set_error(error, "invalid cset destination: %s", operands[0]);
            return false;
        }
        condition = chs_arm64_condition_code(operands[1]);
        if (condition == 0xffu) {
            chs_set_error(error, "unsupported cset condition: %s", operands[1]);
            return false;
        }
        encoded->encoded = chs_arm64_encode_cset(destination.index, condition);
        return true;
    }

    if ((strcmp(mnemonic, "sxtw") == 0 || strcmp(mnemonic, "uxtw") == 0) && operand_count == 2) {
        ChsArm64Register destination;
        ChsArm64Register source;

        if (!chs_arm64_parse_register(operands[0], &destination) || !chs_arm64_parse_register(operands[1], &source)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_extend(strcmp(mnemonic, "sxtw") == 0, destination.index, source.index);
        return true;
    }

    if (strcmp(mnemonic, "mul") == 0 && operand_count == 3) {
        ChsArm64Register destination;
        ChsArm64Register left;
        ChsArm64Register right;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_register(operands[1], &left) ||
            !chs_arm64_parse_register(operands[2], &right)) {
            chs_set_error(error, "invalid mul operands: %s", operands_text);
            return false;
        }
        if (destination.is_64_bit != left.is_64_bit ||
            destination.is_64_bit != right.is_64_bit) {
            chs_set_error(error, "mul register width mismatch: %s", operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_mul(destination.is_64_bit,
                                                destination.index,
                                                left.index,
                                                right.index);
        return true;
    }

    if ((strcmp(mnemonic, "udiv") == 0 || strcmp(mnemonic, "sdiv") == 0) &&
        operand_count == 3) {
        ChsArm64Register destination;
        ChsArm64Register left;
        ChsArm64Register right;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_register(operands[1], &left) ||
            !chs_arm64_parse_register(operands[2], &right)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        if (destination.is_64_bit != left.is_64_bit ||
            destination.is_64_bit != right.is_64_bit) {
            chs_set_error(error, "%s register width mismatch: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_div(strcmp(mnemonic, "sdiv") == 0,
                                                destination.is_64_bit,
                                                destination.index,
                                                left.index,
                                                right.index);
        return true;
    }

    if ((strcmp(mnemonic, "madd") == 0 || strcmp(mnemonic, "msub") == 0) &&
        operand_count == 4) {
        ChsArm64Register destination;
        ChsArm64Register left;
        ChsArm64Register right;
        ChsArm64Register accumulate;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_register(operands[1], &left) ||
            !chs_arm64_parse_register(operands[2], &right) ||
            !chs_arm64_parse_register(operands[3], &accumulate)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        if (destination.is_64_bit != left.is_64_bit ||
            destination.is_64_bit != right.is_64_bit ||
            destination.is_64_bit != accumulate.is_64_bit) {
            chs_set_error(error, "%s register width mismatch: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_madd_msub(strcmp(mnemonic, "msub") == 0,
                                                      destination.is_64_bit,
                                                      destination.index,
                                                      left.index,
                                                      right.index,
                                                      accumulate.index);
        return true;
    }

    if ((strcmp(mnemonic, "lsl") == 0 || strcmp(mnemonic, "lsr") == 0 ||
         strcmp(mnemonic, "asr") == 0) && operand_count == 3) {
        ChsArm64Register destination;
        ChsArm64Register left;
        ChsArm64Register right;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_register(operands[1], &left) ||
            !chs_arm64_parse_register(operands[2], &right)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        if (destination.is_64_bit != left.is_64_bit ||
            destination.is_64_bit != right.is_64_bit) {
            chs_set_error(error, "%s register width mismatch: %s", mnemonic, operands_text);
            return false;
        }
        encoded->encoded = chs_arm64_encode_shift_variable(mnemonic,
                                                           destination.is_64_bit,
                                                           destination.index,
                                                           left.index,
                                                           right.index);
        return true;
    }

    if ((strcmp(mnemonic, "sxtb") == 0 || strcmp(mnemonic, "uxtb") == 0 ||
         strcmp(mnemonic, "sxth") == 0 || strcmp(mnemonic, "uxth") == 0) &&
        operand_count == 2) {
        ChsArm64Register destination;
        ChsArm64Register source;
        bool sign_extend;
        unsigned imms;

        if (!chs_arm64_parse_register(operands[0], &destination) ||
            !chs_arm64_parse_register(operands[1], &source)) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }
        if (destination.is_sp || source.is_sp) {
            chs_set_error(error, "invalid %s operands: %s", mnemonic, operands_text);
            return false;
        }

        sign_extend = (mnemonic[0] == 's');
        imms = (mnemonic[3] == 'b') ? 7u : 15u;
        encoded->encoded = chs_arm64_encode_bitfield_extract(sign_extend,
                                                             destination.is_64_bit,
                                                             destination.index,
                                                             source.index,
                                                             imms);
        return true;
    }

    if (strcmp(mnemonic, "adrp") == 0 && operand_count == 2) {
        ChsArm64Register destination;
        const char *at_page;
        char symbol_name[128];

        if (!chs_arm64_parse_register(operands[0], &destination)) {
            chs_set_error(error, "invalid adrp destination: %s", operands[0]);
            return false;
        }
        at_page = strstr(operands[1], "@PAGE");
        if (at_page == NULL) {
            chs_set_error(error, "adrp requires @PAGE operand: %s", operands[1]);
            return false;
        }
        memcpy(symbol_name, operands[1], (size_t) (at_page - operands[1]));
        symbol_name[at_page - operands[1]] = '\0';
        chs_arm64_copy_trimmed(symbol_name, sizeof(symbol_name), symbol_name);
        encoded->encoded = 0x90000000u | destination.index;
        encoded->uses_symbol = true;
        encoded->symbol_name = strdup(symbol_name);
        encoded->relocation_kind = CHS_RELOC_AARCH64_PAGE21;
        encoded->pc_relative = true;
        return true;
    }

    chs_set_error(error, "unsupported ARM64 instruction: %s %s", mnemonic, operands_text);
    return false;
}

const ChsArchOps chs_arm64_arch_ops = {
    CHS_ARCH_ARM64,
    "arm64",
    8,
    chs_arm64_encode_instruction
};
