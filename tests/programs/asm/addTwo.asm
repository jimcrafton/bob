; Linked into bob programs by the end-to-end tests: a C function written in MASM.
.code
bobTestAddTwo PROC
    lea rax, [rcx + rdx]
    ret
bobTestAddTwo ENDP
END
