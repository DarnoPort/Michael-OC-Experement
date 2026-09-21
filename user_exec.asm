; Michael OS Phase 19: exec() + argv demonstration program.
; It replaces itself with /bin/args-test.elf and passes new arguments.
[BITS 32]

SYS_EXIT  equ 0
SYS_WRITE equ 1
SYS_EXEC  equ 9

section .text
global user_program_start

user_program_start:
    ; SYS_EXEC:
    ;   EBX = path
    ;   ECX = argv pointer
    ;   EDX = argc
    mov eax, SYS_EXEC
    mov ebx, target_path
    mov ecx, exec_argv
    mov edx, 3
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

target_path:
    db "/bin/args-test.elf", 0

exec_argv:
    dd target_path
    dd arg_one
    dd arg_two
    dd 0

arg_one:
    db "exec", 0

arg_two:
    db "phase19", 0

fail_message:
    db "exec-test: SYS_EXEC failed.", 10
fail_message_end:
