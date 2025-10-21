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
; MSVC x64 ABI: 1st arg in RCX (아무 데코레이션 없음)
load_gdt PROC
    ; 만약 호출자가 RDI로 넘긴다면, 아래 한 줄을 풀어라:
    ; mov     rcx, rdi

    ; --- GDT 로드 ---
    lea     rax, GdtTable
    mov     qword ptr [GDTRLabel + 2], rax    ; GDTR.base ← &GdtTable
    lea     rax, GDTRLabel
    lgdt    fword ptr [rax]

    ; 세그먼트 로드 (데이터 세그먼트들)
    mov     ax, 10h            ; data selector
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; CS 갱신: far return 시퀀스
    push    08h                 ; code selector
    lea     rax, after_cs
    push    rax
    retfq

after_cs:
    ; --- (선택) 인터럽트 잠깐 끄기 ---
    cli

    ; --- 전달받은 PML4(=RCX)로 CR3 교체 ---
    mov     rax, rcx
    mov     cr3, rax

    ; --- (옵션) 보호 비트들: 필요하면 주석 해제 ---
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
