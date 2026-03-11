start:
    MOVI32 B15, #0x20000000
    WRSTK B15
    MOVI32 B0, #message
    CALL print
    BRK

print:
    LDBU B1, [B0]
    CMPI32 B1, #0
    BZ done
    ADDI8 B0, #1
    J print
done:
    RET

message:
    .byte "O", "K", 0
