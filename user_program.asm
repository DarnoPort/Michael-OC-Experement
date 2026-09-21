; NanoOS Phase 12 user program.
; Loaded as an ELF32 PT_LOAD image at 0x80000000.
;
; Demonstrates:
; - Ring 3 syscalls
; - per-process PID
; - per-process user heap through sbrk
; - scheduler yield
; - preemptive timer switching

[BITS 32]

SYS_EXIT   equ 0
SYS_WRITE  equ 1
SYS_GETPID equ 2
SYS_YIELD  equ 3
SYS_SBRK   equ 4

USER_HEAP_BYTES equ 4096

section .text
global user_program_start

user_program_start:
    ; Allocate one private heap page.
    mov eax, SYS_SBRK
    mov ebx, USER_HEAP_BYTES
    int 0x80

    cmp eax, 0xFFFFFFFF
    je .exit

    mov esi, eax
    mov edi, 8

.loop:
    ; Get current PID.
    mov eax, SYS_GETPID
    int 0x80

    ; Format a one-digit demo PID followed by newline.
    ; Phase 12 only creates low PIDs, so one byte is sufficient here.
    add al, '0'
    mov byte [esi], al
    mov byte [esi + 1], 10

    ; Write from the process-private heap.
    mov eax, SYS_WRITE
    mov ebx, esi
    mov ecx, 2
    int 0x80

    ; Intentional CPU load so the timer can preempt us.
    mov ecx, 5000000
.delay:
    dec ecx
    jnz .delay

    ; Also demonstrate voluntary scheduling.
    mov eax, SYS_YIELD
    int 0x80

    dec edi
    jnz .loop

.exit:
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang
