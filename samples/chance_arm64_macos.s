.build_version macos, 15, 0

.extern __cert__box_ptr
.extern _Std_IO_printnl_ptr_to_char
.extern __cert__null_deref_fallback
.extern __cert__box_i64

.section __DATA,__const
.p2align 0
__ccb_str_file_0:
    .byte 0x74, 0x65, 0x73, 0x74, 0x2e, 0x63, 0x65, 0x00

.p2align 0
__ccb_str_name_1:
    .byte 0x78, 0x00

.file 0 "/Users/nathanhornby/aqtools" "test.ce"
.file 1 "/Users/nathanhornby/aqtools/test.ce"

.section __TEXT,__text,regular,pure_instructions

.p2align 2
__cert__entry_main:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    stp x27, x28, [sp, #-16]!
    sub sp, sp, #64
    mov x27, sp
    str x0, [x27, #0]
    str x1, [x27, #8]
    .loc 1 7 13
    .loc 1 7 9
    movz w9, #0x123
    str w9, [x27, #16]
    .loc 1 8 25
    add x9, x27, #16
    str x9, [x27, #32]
    ldr x0, [x27, #32]
    adrp x1, ___cert__entry_main____str0@PAGE
    add x1, x1, ___cert__entry_main____str0@PAGEOFF
    bl __cert__box_ptr
    str x0, [x27, #32]
    .loc 1 8 17
    ldr x9, [x27, #32]
    str x9, [x27, #24]
    .loc 1 10 5
    ldr x0, [x27, #24]
    bl _Test_set_type_kind_16
    .loc 1 12 16
    .loc 1 12 40
    .loc 1 12 43
    adrp x11, ___cert__entry_main____str1@PAGE
    add x11, x11, ___cert__entry_main____str1@PAGEOFF
    str x11, [x27, #32]
    ldr x9, [x27, #24]
    ldr x10, [x9]
    str x10, [x27, #48]
    .loc 1 12 8
    ldr x0, [x27, #32]
    ldr w1, [x27, #16]
    ldr x2, [x27, #48]
    sub sp, sp, #16
    ldr w9, [x27, #16]
    sxtw x9, w9
    str x9, [sp, #0]
    ldr x9, [x27, #48]
    str x9, [sp, #8]
    mov x15, sp
    bl _Std_IO_printnl_ptr_to_char
    add sp, sp, #16
    str w0, [x27, #32]
    .loc 1 13 9
    .loc 1 13 5
    mov w0, wzr
    mov sp, x27
    add sp, sp, #64
    ldp x27, x28, [sp], #16
    ldp x29, x30, [sp], #16
    ret

.p2align 2
_Test_set_type_kind_16:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    stp x27, x28, [sp, #-16]!
    sub sp, sp, #64
    mov x27, sp
    str x0, [x27, #0]
    .loc 1 17 13
    .loc 1 17 9
    movz w9, #0x456
    str w9, [x27, #8]
    .loc 1 18 7
    ldr x9, [x27, #0]
    cmp x9, xzr
    cset w11, eq
    strb w11, [x27, #32]
    ldrb w9, [x27, #32]
    cmp w9, wzr
    b.ne LTest_set_type_kind_16__Lcc_nullchk_call_0
    b LTest_set_type_kind_16__Lcc_nullchk_cont_1
LTest_set_type_kind_16__Lcc_nullchk_call_0:
    adrp x9, __ccb_str_file_0@PAGE
    add x9, x9, __ccb_str_file_0@PAGEOFF
    str x9, [x27, #24]
    movz x11, #0x12
    str x11, [x27, #32]
    adrp x9, __ccb_str_name_1@PAGE
    add x9, x9, __ccb_str_name_1@PAGEOFF
    str x9, [x27, #40]
    ldr x0, [x27, #24]
    ldr x1, [x27, #32]
    ldr x2, [x27, #40]
    bl __cert__null_deref_fallback
    str x0, [x27, #24]
LTest_set_type_kind_16__Lcc_nullchk_cont_1:
    .loc 1 18 9
    ldr w9, [x27, #8]
    sxtw x9, w9
    str x9, [x27, #32]
    ldr x0, [x27, #32]
    adrp x1, __Test_set_type_kind_16____str2@PAGE
    add x1, x1, __Test_set_type_kind_16____str2@PAGEOFF
    bl __cert__box_i64
    str x0, [x27, #32]
    .loc 1 18 7
    ldr x9, [x27, #32]
    str x9, [x27, #16]
    ldr x9, [x27, #0]
    ldr x10, [x27, #16]
    str x10, [x9]
    mov sp, x27
    add sp, sp, #64
    ldp x27, x28, [sp], #16
    ldp x29, x30, [sp], #16
    ret

.globl _main
.p2align 2
_main:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    stp x27, x28, [sp, #-16]!
    sub sp, sp, #32
    mov x27, sp
    str w0, [x27, #0]
    str x1, [x27, #8]
    ldr w9, [x27, #0]
    uxtw x9, w9
    str x9, [x27, #24]
    ldr x0, [x27, #8]
    ldr x1, [x27, #24]
    bl __cert__entry_main
    str w0, [x27, #16]
    ldr w0, [x27, #16]
    mov sp, x27
    add sp, sp, #32
    ldp x27, x28, [sp], #16
    ldp x29, x30, [sp], #16
    ret

.section __TEXT,__cstring
___cert__entry_main____str0:
    .byte 0x70, 0x74, 0x72, 0x00
___cert__entry_main____str1:
    .byte 0x78, 0x20, 0x69, 0x73, 0x20, 0x25, 0x78, 0x2c, 0x20, 0x78, 0x5f, 0x72, 0x65, 0x66, 0x20, 0x69, 0x73, 0x20, 0x25, 0x78, 0x00
__Test_set_type_kind_16____str2:
    .byte 0x69, 0x33, 0x32, 0x00