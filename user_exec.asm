; Michael OS Phase 17: exec() demonstration program.
; It replaces itself with /bin/demo.elf.
[BITS 32]

SYS_EXIT  equ 0
SYS_WRITE equ 1
SYS_EXEC  equ 9

section .text
global user_program_start

user_program_start:
    mov eax, SYS_EXEC
    mov ebx, demo_path
    int 0x80

    ; exec() only returns on failure.
    mov eax, SYS_WRITE
    mov ebx, fail_message
    mov ecx, fail_message_end - fail_message
    int 0x80

    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang

demo_path:
    db "/bin/demo.elf", 0

fail_message:
    db "exec-test: SYS_EXEC failed.", 10
fail_message_end:
