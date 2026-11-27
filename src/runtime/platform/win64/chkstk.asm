; __chkstk: probes each 4 KB page of a large stack allocation so the guard page
; is hit in order. RAX = allocation size; the caller adjusts RSP itself.
; Preserves every register (Win64 contract for __chkstk).

_TEXT SEGMENT

PUBLIC __chkstk

__chkstk PROC
    push    rcx
    push    rax
    cmp     rax, 1000h
    lea     rcx, [rsp + 24]
    jb      lastPage
probeLoop:
    sub     rcx, 1000h
    test    qword ptr [rcx], rcx
    sub     rax, 1000h
    cmp     rax, 1000h
    ja      probeLoop
lastPage:
    sub     rcx, rax
    test    qword ptr [rcx], rcx
    pop     rax
    pop     rcx
    ret
__chkstk ENDP

_TEXT ENDS

END
