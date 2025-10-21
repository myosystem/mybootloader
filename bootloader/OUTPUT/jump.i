.code
jump_to_address PROC
    mov rsp, rcx
    jmp rdx
jump_to_address ENDP
END
