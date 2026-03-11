#include "chs/arch.h"

extern const ChsArchOps chs_arm64_arch_ops;
extern const ChsArchOps chs_x86_64_arch_ops;

const ChsArchOps *chs_find_arch_ops(ChsArchKind kind) {
    switch (kind) {
        case CHS_ARCH_ARM64:
            return &chs_arm64_arch_ops;
        case CHS_ARCH_X86_64:
            return &chs_x86_64_arch_ops;
        case CHS_ARCH_BSLASH:
            return NULL;
    }

    return NULL;
}
