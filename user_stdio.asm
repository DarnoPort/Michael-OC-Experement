; Michael OS Phase 26.1: standard streams test.
; stdin = fd 0
; stdout = fd 1
;
; SYS_READ is non-blocking in this phase. The program yields
; while no keyboard byte is currently available, then reads a
; line from stdin and prints what was received through stdout.

[BITS 32]

SYS_EXIT     equ 0
SYS_YIELD    equ 3
SYS_READ     equ 10
SYS_FD_WRITE equ 11

STDIN  equ 0
STDOUT equ 1

section .text
global user_program_start

user_program_start:
    mov ebx, banner
    call write_cstr

.read_loop:
    mov eax, SYS_READ
    mov ebx, STDIN
    mov ecx, input_char
    mov edx, 1
    int 0x80

    cmp eax, 0xFFFFFFFF
    je .exit

    cmp eax, 0
    je .yield

    cmp byte [input_char], 10
    je .got_line

    mov eax, [input_length]
    cmp eax, 63
    jae .exit

    mov dl, [input_char]
    mov [input_buffer + eax], dl
    inc eax
    mov [input_length], eax
    jmp .read_loop

.yield:
    mov eax, SYS_YIELD
    int 0x80
    jmp .read_loop

.got_line:
    mov ebx, received
    call write_cstr

    mov eax, SYS_FD_WRITE
    mov ebx, STDOUT
    mov ecx, input_buffer
    mov edx, [input_length]
    int 0x80

    mov ebx, newline
    call write_cstr

.exit:
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang

write_cstr:
    pushad
    mov esi, ebx
    xor ecx, ecx
.count:
    cmp byte [esi + ecx], 0
    je .count_done
    inc ecx
    jmp .count
.count_done:
    mov edx, ecx
    mov eax, SYS_FD_WRITE
    mov ebx, STDOUT
    mov ecx, esi
    int 0x80
    popad
    ret

banner:
    db 10, "[stdio-test] Type a line and press Enter: ", 0

received:
    db 10, "[stdin fd=0] read: ", 0

newline:
    db 10, 0

section .data
input_length:
    dd 0

section .bss
input_buffer:
    resb 64

input_char:
    resb 1
