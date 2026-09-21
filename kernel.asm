; kernel.asm — Этап 1: Базовая 16-битная ОС (512 байт)
[BITS 16]
[ORG 0x7C00]            ; Стандартный адрес загрузки BIOS

start:
    cli                 ; Отключаем прерывания на время настройки сегментов
    xor ax, ax          ; Обнуляем регистр AX
    mov ds, ax          ; Настраиваем сегменты данных
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00      ; Настраиваем вершину стека
    sti                 ; Включаем прерывания обратно

    ; Очистка экрана перед стартом
    mov ax, 0x0003
    int 0x10

    mov si, msg_welcome
    call print_string

prompt_loop:
    mov si, msg_prompt
    call print_string
    call read_command
    call process_command
    jmp prompt_loop

; --- ФУНКЦИЯ: Вывод строки на экран ---
print_string:
    lodsb               ; Загружаем следующий байт из SI в AL
    or al, al           ; Проверяем, не ноль ли это (конец строки)
    jz .done
    mov ah, 0x0E        ; Функция BIOS: вывод символа (Teletype)
    mov bh, 0x00        ; Нулевая страница видеопамяти
    mov bl, 0x07        ; Цвет (Светло-серый на черном)
    int 0x10            ; Вызов прерывания видео
    jmp print_string
.done:
    ret

; --- ФУНКЦИЯ: Чтение ввода пользователя ---
read_command:
    mov di, buffer      ; Будем записывать ввод в буфер
.loop:
    xor ah, ah          ; Функция BIOS: ждать нажатия клавиши
    int 0x16            ; Вызов прерывания клавиатуры
    cmp al, 13          ; Нажали Enter? (ASCII 13)
    je .done
    cmp al, 8           ; Нажали Backspace? (ASCII 8)
    je .backspace
    
    ; Эхо символа на экран и сохранение в буфер
    mov ah, 0x0E
    int 0x10
    stosb               ; Сохраняем AL по адресу DI и сдвигаем DI
    jmp .loop

.backspace:
    cmp di, buffer      ; Если мы в начале буфера, стирать нечего
    jbe .loop
    dec di              ; Уменьшаем указатель буфера
    mov ah, 0x0E
    mov al, 8           ; Шаг назад
    int 0x10
    mov al, 32          ; Пробел (затираем символ)
    int 0x10
    mov al, 8           ; Снова шаг назад
    int 0x10
    jmp .loop

.done:
    xor al, al          ; Записываем 0 (конец строки) в буфер
    stosb
    ; Перенос строки на экране
    mov ah, 0x0E
    mov al, 13          ; Возврат каретки
    int 0x10
    mov al, 10          ; Новая строка
    int 0x10
    ret

; --- ФУНКЦИЯ: Обработка команд ---
process_command:
    mov si, buffer
    cmp byte [si], 0    ; Если ввели пустую строку
    je .end
    cmp byte [si], 'h'  ; Команда начинается на 'h'? (help)
    je .help
    cmp byte [si], 'c'  ; Команда начинается на 'c'? (clear)
    je .clear

    ; Если команда не распознана
    mov si, msg_unknown
    call print_string
    ret

.help:
    mov si, msg_help
    call print_string
    ret

.clear:
    mov ax, 0x0003      ; Установка видеорежима (очищает экран)
    int 0x10
    ret
.end:
    ret

; --- ДАННЫЕ (Тексты и буферы) ---
msg_welcome db "=== Welcome to NanoOS v0.1 ===", 13, 10, "System booted successfully.", 13, 10, 0
msg_prompt  db "> ", 0
msg_help    db "Commands: [h]elp, [c]lear screen", 13, 10, 0
msg_unknown db "Unknown command. Press 'h' for help.", 13, 10, 0
buffer      times 64 db 0

; --- ЗАВЕРШЕНИЕ СЕКТОРА ---
times 510 - ($ - $$) db 0   ; Добиваем остаток сектора нулями до 510 байт
dw 0xAA55                   ; Магическая сигнатура загрузочного сектора (Boot Signature)
