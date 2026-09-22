; Michael OS Phase 18: argc/argv demonstration.
; The kernel builds a small process startup stack:
;   [esp + 0]  argc
;   [esp + 4]  argv
;   [esp + 8]  envp (currently NULL)
;
; argv[argc] is also NULL-terminated.
[BITS 32]

SYS_EXIT  equ 0
SYS_WRITE equ 1
SYS_FD_WRITE equ 11

section .text
global user_program_start

user_program_start:
    mov esi, esp
    mov edi, [esi]          ; argc
    mov ebp, [esi + 4]      ; argv

    mov ebx, title
    call write_cstr

    mov ebx, argc_label
    call write_cstr

    mov eax, edi
    add al, '0'
    mov [digit_buffer], al
    mov byte [digit_buffer + 1], 10
    mov ebx, digit_buffer
    mov ecx, 2
    call write_buffer

    xor ecx, ecx            ; argv index

.print_arg:
    cmp ecx, edi
    jae .done

    mov ebx, arg_prefix
    call write_cstr

    mov eax, ecx
    add al, '0'
    mov [digit_buffer], al
    mov byte [digit_buffer + 1], 0
    mov ebx, digit_buffer
    call write_cstr

    mov ebx, arg_separator
    call write_cstr

    mov edx, [ebp + ecx * 4]
    mov ebx, edx
    call write_cstr

    mov ebx, newline
    call write_cstr

    inc ecx
    jmp .print_arg

.done:
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang

; EBX = pointer to zero-terminated user string.
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
    mov ebx, 1
    mov ecx, esi
    int 0x80
    popad
    ret

; EBX = pointer, ECX = byte count.
write_buffer:
    pushad
    mov edx, ecx
    mov ecx, ebx
    mov ebx, 1
    mov eax, SYS_FD_WRITE
    int 0x80
    popad
    ret

title:
    db "=== argv test ===", 10, 0

argc_label:
    db "argc=", 0

arg_prefix:
    db "argv[", 0

arg_separator:
    db "]=", 0

newline:
    db 10, 0

section .bss
digit_buffer:
    resb 2
