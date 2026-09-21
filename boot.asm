; NanoOS Multiboot entry point.
; GRUB enters the kernel in 32-bit protected mode, but we install
; our own flat GDT instead of relying on GRUB's temporary GDT.

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
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main

_start:
    cli

    ; Save the Multiboot values in registers while we replace the stack.
    ; EAX = Multiboot magic, EBX = pointer to multiboot_info.
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

    ; cdecl: kernel_main(multiboot_magic, multiboot_info_addr)
    push edi
    push esi
    call kernel_main
    add esp, 8

.hang:
    cli
    hlt
    jmp .hang

align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start
