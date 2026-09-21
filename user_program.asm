; Ring 3 scheduler demo for NanoOS Phase 11.
;
; Each process has private physical code/stack pages. The scheduler maps
; the currently running process into the fixed user virtual addresses below.

[BITS 32]

SYS_EXIT   equ 0
SYS_WRITE  equ 1
SYS_GETPID equ 2

USER_CODE_BASE equ 0x80000000
USER_STACK_TOP equ 0x80002000

section .text
global user_program_start
global user_program_end

user_program_start:
    mov edi, 5

.loop:
    ; Ask the kernel for our PID.
    mov eax, SYS_GETPID
    int 0x80

    ; Put "<pid>\n" at the end of our private user stack page.
    add al, '0'
    mov byte [USER_STACK_TOP - 2], al
    mov byte [USER_STACK_TOP - 1], 10

    ; Write the two-byte message.
    mov eax, SYS_WRITE
    mov ebx, USER_STACK_TOP - 2
    mov ecx, 2
    int 0x80

    ; Busy work is intentional: the 100 Hz timer can preempt this process.
    mov ecx, 10000000
.delay:
    dec ecx
    jnz .delay

    dec edi
    jnz .loop

    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang

user_program_end:
