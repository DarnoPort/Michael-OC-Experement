; Tiny user-mode test program for NanoOS Phase 10.
[BITS 32]

USER_CODE_BASE equ 0x80000000

section .text
global user_program_start
global user_program_end

user_program_start:
    mov eax, 1
    mov ebx, USER_CODE_BASE + user_message - user_program_start
    mov ecx, user_message_end - user_message
    int 0x80

    mov eax, 0
    int 0x80

.hang:
    jmp .hang

user_message:
    db "Hello from Ring 3! System call works."
    db 10

user_message_end:
    db 0

user_program_end:
