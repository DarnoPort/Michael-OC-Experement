; NanoOS 32-bit interrupt stubs.
[BITS 32]

section .text

global load_idt
global keyboard_handler_asm
global timer_handler_asm
global syscall_handler_asm
global dummy_handler_asm

extern keyboard_handler_c
extern timer_handler_c
extern exception_handler_c
extern syscall_dispatch
extern scheduler_on_timer
extern scheduler_on_syscall
extern scheduler_on_exec
extern user_exit_stub

load_idt:
    mov edx, [esp + 4]
    lidt [edx]
    ret

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
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
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

isr_common_noerr:
    pushad
    mov eax, [esp + 32]
    lea edx, [esp + 36]
    push edx
    push dword 0
    push eax
    call exception_handler_c
    add esp, 12
    popad
    add esp, 4
    iretd

isr_common_err:
    pushad
    mov eax, [esp + 32]
    mov edx, [esp + 36]
    lea ecx, [esp + 40]
    push ecx
    push edx
    push eax
    call exception_handler_c
    add esp, 12
    popad
    add esp, 8
    iretd

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

timer_handler_asm:
    pushad
    cld
    call timer_handler_c

    push esp
    call scheduler_on_timer
    add esp, 4

    test eax, eax
    jz .timer_no_switch

    mov edx, eax

    mov al, 0x20
    out 0x20, al

    mov esp, edx
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popad
    iretd

.timer_no_switch:
    mov al, 0x20
    out 0x20, al

    popad
    iretd

keyboard_handler_asm:
    pushad
    cld
    call keyboard_handler_c
    popad
    mov al, 0x20
    out 0x20, al
    iretd

; System call interrupt 0x80.
;
; syscall_dispatch() returns:
;   0 = ordinary syscall, return to the same user context
;   1 = return directly to the kernel shell
;   2 = run the scheduler and switch/restore a process context
;   3 = exec() replaced the current user image
syscall_handler_asm:
    pushad
    cld

    push esp
    call syscall_dispatch
    add esp, 4

    cmp eax, 2
    je .schedule

    cmp eax, 3
    je .exec

    cmp eax, 1
    je .exit_to_kernel

    popad
    iretd

.exec:
    call scheduler_on_exec

    test eax, eax
    jz .exit_to_kernel

    mov edx, eax
    mov esp, edx
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popad
    iretd

.schedule:
    push esp
    call scheduler_on_syscall
    add esp, 4

    test eax, eax
    jz .exit_to_kernel

    mov edx, eax
    mov esp, edx
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popad
    iretd

.exit_to_kernel:
    popad

    mov eax, user_exit_stub
    mov [esp], eax
    mov dword [esp + 4], 0x08

    iretd

irq_common:
    pushad
    mov eax, [esp + 32]

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

dummy_handler_asm:
    cli
.dummy_halt:
    hlt
    jmp .dummy_halt
