; bob_msgSend(self, selector, args...) -> jumps to the implementation.
; RCX = self, RDX = selector; every other argument register and the stack are
; left untouched so the implementation sees the caller's arguments unchanged.
; Fast path: probe the receiver class's cache; slow path: bobMsgSendLookup.

EXTERN bobMsgSendLookup:PROC

_TEXT SEGMENT

PUBLIC bob_msgSend

; BobClass.cache is at offset 48; BobMethodCache = { mask, entries[{selector, imp}] }
bob_msgSend PROC
    mov     r10, qword ptr [rcx]            ; isa
    mov     r11, qword ptr [r10 + 48]       ; cache
    test    r11, r11
    jz      lookup
    mov     rax, rdx
    shr     rax, 3
probe:
    and     rax, qword ptr [r11]            ; index &= mask
    mov     r10, rax
    shl     r10, 4
    lea     r10, [r11 + r10 + 8]            ; &entries[index]
    cmp     qword ptr [r10], rdx
    je      hit
    cmp     qword ptr [r10], 0
    je      lookupFromCache
    inc     rax
    jmp     probe
hit:
    jmp     qword ptr [r10 + 8]
lookupFromCache:
    mov     r10, qword ptr [rcx]
lookup:
    ; 136 bytes: 32 shadow + 32 GPRs + 64 XMM + 8 to align RSP to 16.
    sub     rsp, 136
    mov     qword ptr [rsp + 32], rcx
    mov     qword ptr [rsp + 40], rdx
    mov     qword ptr [rsp + 48], r8
    mov     qword ptr [rsp + 56], r9
    movdqu  xmmword ptr [rsp + 64], xmm0
    movdqu  xmmword ptr [rsp + 80], xmm1
    movdqu  xmmword ptr [rsp + 96], xmm2
    movdqu  xmmword ptr [rsp + 112], xmm3
    mov     rcx, qword ptr [rcx]            ; isa
    call    bobMsgSendLookup                ; (isa, selector) -> imp in RAX
    mov     rcx, qword ptr [rsp + 32]
    mov     rdx, qword ptr [rsp + 40]
    mov     r8, qword ptr [rsp + 48]
    mov     r9, qword ptr [rsp + 56]
    movdqu  xmm0, xmmword ptr [rsp + 64]
    movdqu  xmm1, xmmword ptr [rsp + 80]
    movdqu  xmm2, xmmword ptr [rsp + 96]
    movdqu  xmm3, xmmword ptr [rsp + 112]
    add     rsp, 136
    jmp     rax
bob_msgSend ENDP

_TEXT ENDS

END
