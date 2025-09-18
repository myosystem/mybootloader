PUBLIC load_gdt

.data
ALIGN 8
GdtTable        dq 0
                dq 0x00AF9A000000FFFF     ; 0x08: 64-bit code
                dq 0x00AF92000000FFFF     ; 0x10: 64-bit data
                dq 0x00AFFA000000FFFF
                dq 0x00AFF2000000FFFF
GdtTableEnd:

GDTRLabel       dw GdtTableEnd - GdtTable - 1
                dq 0

.code
; Prototype: void load_gdt(UINT64 pml4_phys);
; MSVC x64 ABI: 1st arg in RCX
load_gdt PROC
    ; mov     rcx, rdi

    lea     rax, GdtTable
    mov     qword ptr [GDTRLabel + 2], rax    ; GDTR.base ← &GdtTable
    lea     rax, GDTRLabel
    lgdt    fword ptr [rax]

    mov     ax, 10h            ; data selector
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    push    08h                 ; code selector
    lea     rax, after_cs
    push    rax
    retfq

after_cs:
    cli

    mov     rax, rcx
    mov     cr3, rax

    ; mov     rax, cr0
    ; bts     rax, 16            ; CR0.WP = 1
    ; mov     cr0, rax
    ; mov     ecx, 0C0000080h    ; MSR_IA32_EFER
    ; rdmsr                      ; EDX:EAX ← EFER
    ; bts     eax, 11            ; EFER.NXE = 1
    ; wrmsr

    ret
load_gdt ENDP
END
