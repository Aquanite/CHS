#include "chs/arch.h"

static bool chs_x86_64_encode_instruction(const ChsObject *object,
                                          const ChsSection *section,
                                          uint64_t section_offset,
                                          const char *mnemonic,
                                          const char *operands,
                                          ChsEncodedInstruction *encoded,
                                          ChsError *error) {
    (void) object;
    (void) section;
    (void) section_offset;
    (void) operands;
    (void) encoded;
    chs_set_error(error, "x86_64 encoder not implemented yet for instruction '%s'", mnemonic);
    return false;
}

const ChsArchOps chs_x86_64_arch_ops = {
    CHS_ARCH_X86_64,
    "x86_64",
    8,
    chs_x86_64_encode_instruction
};
