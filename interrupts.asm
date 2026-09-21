; NanoOS 32-bit interrupt stubs.
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

; Keyboard IRQ1.
keyboard_handler_asm:
    pushad
    cld
    call keyboard_handler_c
    popad
    mov al, 0x20
    out 0x20, al
    iretd

; Safe fallback for unexpected interrupts/exceptions.
; The CPU pushes an error code for exceptions 8, 10-14, 17 and 30.
; We inspect the vector pushed by the individual stubs and remove the
; error code when necessary before IRET.
dummy_handler_asm:
    pushad
    mov eax, [esp + 32]

    ; Acknowledge PIC IRQs 8..15.
    cmp eax, 40
    jb .no_slave_eoi
    mov al, 0x20
    out 0xA0, al
.no_slave_eoi:
    cmp eax, 32
    jb .no_master_eoi
    cmp eax, 48
    jae .no_master_eoi
    mov al, 0x20
    out 0x20, al
.no_master_eoi:

    ; Remove saved registers.
    popad

    ; CPU error-code exceptions.
    cmp eax, 8
    je .discard_error
    cmp eax, 10
    jb .return
    cmp eax, 14
    jbe .discard_error
    cmp eax, 17
    je .discard_error
    cmp eax, 30
    je .discard_error

.return:
    add esp, 4
    iretd

.discard_error:
    add esp, 8
    iretd

; Each stub pushes its vector number.
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword %1
    jmp dummy_handler_asm
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp dummy_handler_asm
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR 29
ISR_ERR 30
ISR_NOERR 31