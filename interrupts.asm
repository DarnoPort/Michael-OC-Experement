[BITS 32]
section .text           ; ВАЖНО: Указываем, что это исполняемый код

global load_idt
global keyboard_handler_asm
global dummy_handler_asm
extern keyboard_handler_c

load_idt:
    mov edx, [esp + 4]
    lidt [edx]
    ret

; Обработчик клавиатуры
keyboard_handler_asm:
    pushad
    cld                     ; Очищаем флаг направления (важно для языка Си)
    call keyboard_handler_c
    popad
    iretd

; Заглушка для ВСЕХ остальных прерываний (таймер, мышь и случайные шумы)
dummy_handler_asm:
    pushad
    ; Отправляем сигнал EOI (End of Interrupt) в контроллер
    mov al, 0x20
    out 0x20, al
    popad
    iretd