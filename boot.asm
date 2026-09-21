; Michael OS Multiboot entry point.
; Phase 10/11: user mode, TSS and process scheduling.

MBALIGN  equ  1 << 0
MEMINFO  equ  1 << 1
FLAGS    equ  MBALIGN | MEMINFO
MAGIC    equ  0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 16
global stack_bottom
global stack_top
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
global set_tss_descriptor
global load_tss
global enter_user_mode
global user_exit_stub

extern kernel_main
extern user_return_esp
extern user_return_ebp
extern user_return_ebx
extern user_return_esi
extern user_return_edi

_start:
    cli
    mov esi, eax
    mov edi, ebx

    lgdt [gdt_descriptor]
    jmp 0x08:.gdt_loaded

.gdt_loaded:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, stack_top
    xor ebp, ebp

    push edi
    push esi
    call kernel_main
    add esp, 8

.hang:
    cli
    hlt
    jmp .hang

; [esp+4] = TSS base, [esp+8] = TSS limit.
set_tss_descriptor:
    mov eax, [esp + 4]
    mov edx, [esp + 8]

    mov word [gdt_tss + 0], dx
    mov word [gdt_tss + 2], ax

    shr eax, 16
    mov byte [gdt_tss + 4], al
    mov byte [gdt_tss + 5], 0x89
    mov byte [gdt_tss + 6], 0x00
    mov byte [gdt_tss + 7], ah
    ret

load_tss:
    mov ax, 0x28
    ltr ax
    ret

; [esp+4] = user EIP, [esp+8] = user ESP.
enter_user_mode:
    ; Preserve the kernel caller's callee-saved registers.
    ; The Ring 3 program is free to change these registers.
    mov [user_return_esp], esp
    mov [user_return_ebp], ebp
    mov [user_return_ebx], ebx
    mov [user_return_esi], esi
    mov [user_return_edi], edi

    mov ecx, [esp + 4]
    mov edx, [esp + 8]

    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push dword 0x23
    push edx
    pushfd
    or dword [esp], 0x200
    push dword 0x1B
    push ecx
    iretd

user_exit_stub:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov esp, [user_return_esp]
    ret

section .data
align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF       ; 0x08 kernel code
    dq 0x00CF92000000FFFF       ; 0x10 kernel data
    dq 0x00CFFA000000FFFF       ; 0x18 user code, DPL3
    dq 0x00CFF2000000FFFF       ; 0x20 user data, DPL3
gdt_tss:
    dq 0x0000000000000000       ; 0x28 TSS, filled at runtime
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start
