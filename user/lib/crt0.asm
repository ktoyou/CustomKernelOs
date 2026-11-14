; Точка входа динамической программы. Ядро уже запустило ld.so, а тот
; прыгает сюда со стеком Linux: argc, argv, envp, auxv.
bits 32
global _start
extern __libc_start

section .text
_start:
    mov eax, [esp]
    lea ecx, [esp + 4]
    lea edx, [ecx + eax * 4 + 4]
    push edx
    push ecx
    push eax
    call __libc_start
    jmp $
