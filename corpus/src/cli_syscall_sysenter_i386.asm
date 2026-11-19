; Standard i386 Linux sysenter trampoline helper (NASM elf32)
; uint32_t nm_sysenter_call(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3)

section .text
global nm_sysenter_call

nm_sysenter_call:
    push ebx
    push esi
    push edi
    push ebp
    mov eax, [esp + 20]
    mov ebx, [esp + 24]
    mov ecx, [esp + 28]
    mov edx, [esp + 32]
    call .do_sysenter
    jmp .done
.do_sysenter:
    push ecx
    push edx
    push ebp
    mov ebp, esp
    sysenter
    pop ebp
    pop edx
    pop ecx
    ret
.done:
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
