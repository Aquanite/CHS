start:
    MOVI32 B0, #local_data
    MOVI32 B1, #extern_word
    CALL extern_call
    BZ extern_short
    BRK

local_data:
    .byte extern_byte
    .word extern_word
    .dword extern_dword
    .qword extern_qword
