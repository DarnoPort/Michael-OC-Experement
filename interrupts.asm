; NanoOS 32-bit interrupt stubs.
[BITS 32]

section .text

global load_idt
global keyboard_handler_asm
global timer_handler_asm
global dummy_handler_asm

extern keyboard_handler_c
extern timer_handler_c
extern exception_handler_c

load_idt:
    mov edx, [esp + 4]
    lidt [edx]
    ret

; -----------------------------------------------------------------------------
; CPU exceptions 0..31
; -----------------------------------------------------------------------------

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common_noerr
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common_err
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
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
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; no-error exception stack:
;   vector, EIP, CS, EFLAGS
isr_common_noerr:
    pushad
    mov eax, [esp + 32]
    push dword 0
    push eax
    call exception_handler_c
    add esp, 8
    popad
    add esp, 4
    iretd

; error-code exception stack:
;   vector, error, EIP, CS, EFLAGS
isr_common_err:
    pushad
    mov eax, [esp + 32]
    mov edx, [esp + 36]
    push edx
    push eax
    call exception_handler_c
    add esp, 8
    popad
    add esp, 8
    iretd

; -----------------------------------------------------------------------------
; Hardware IRQs 0..15 -> IDT vectors 32..47
; -----------------------------------------------------------------------------

%macro IRQ 1
global irq%1
irq%1:
    push dword %1
    jmp irq_common
%endmacro

IRQ 32
IRQ 33
IRQ 34
IRQ 35
IRQ 36
IRQ 37
IRQ 38
IRQ 39
IRQ 40
IRQ 41
IRQ 42
IRQ 43
IRQ 44
IRQ 45
IRQ 46
IRQ 47

; Timer IRQ0.
timer_handler_asm:
    pushad
    cld
    call timer_handler_c
    popad
    mov al, 0x20
    out 0x20, al
    iretd

; Keyboard IRQ1. EOI is sent here, not in the C handler.
keyboard_handler_asm:
    pushad
    cld
    call keyboard_handler_c
    popad
    mov al, 0x20
    out 0x20, al
    iretd

; Generic IRQ handler for hardware interrupts without a dedicated driver yet.
irq_common:
    pushad
    mov eax, [esp + 32]

    ; Slave PIC IRQs 8..15 are vectors 40..47.
    cmp eax, 40
    jb .master_eoi
    mov al, 0x20
    out 0xA0, al

.master_eoi:
    mov al, 0x20
    out 0x20, al

    popad
    add esp, 4
    iretd

; Kept for compatibility with older sources; unused by the current IDT.
dummy_handler_asm:
    cli
.dummy_halt:
    hlt
    jmp .dummy_halt
