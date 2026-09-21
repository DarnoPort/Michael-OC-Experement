; NanoOS 32-bit interrupt handlers.
[BITS 32]

section .text
global load_idt
global keyboard_handler_asm
global dummy_handler_asm
extern keyboard_handler_c

load_idt:
    mov edx, [esp + 4]
    lidt [edx]
    ret

; IRQ1 keyboard handler.
keyboard_handler_asm:
    pushad
    cld
    call keyboard_handler_c
    popad
    mov al, 0x20
    out 0x20, al
    iretd

; Temporary fallback for all other IDT entries.
; These entries are only installed as a safety net for now.
; CPU exceptions should get dedicated panic handlers later.
dummy_handler_asm:
    pushad
    mov al, 0x20
    out 0x20, al
    popad
    iretd
