# NanoOS

Учебная 32-битная x86 ОС.

Текущий этап:
- GRUB Multiboot
- собственная flat GDT
- IDT на 256 записей
- PIC 8259A
- PIT timer 100 Hz
- клавиатура через IRQ1
- VGA text mode 80x25 со скроллингом
- физический allocator страниц 4 КБ на основе Multiboot memory map
- kernel heap с `malloc/free`
- shell: help, clear, uptime, ticks, meminfo, physinfo, memtest

## Phase 8

Физический allocator использует bitmap: один бит соответствует одной странице
размером 4096 байт. Вначале все страницы считаются занятыми, затем области типа
`available` из Multiboot memory map помечаются свободными.

Перед выдачей памяти резервируются:
- первые 1 МБ;
- область ядра;
- VGA memory;
- Multiboot information и сама memory map;
- bitmap физического allocator'а, потому что она находится внутри BSS ядра.

Kernel heap использует физический allocator как источник непрерывных страниц.
Поверх этих страниц работает first-fit allocator с разделением и слиянием
свободных блоков.

Пока paging ещё не реализован, ядро использует физические адреса напрямую.
Поэтому непрерывность физической памяти важна для kernel heap.

## Проверка

Сборка:

```bash
make clean
make
make iso
make run
```

В shell:

```text
meminfo
physinfo
memtest
```

`memtest` выделяет несколько блоков через `malloc`, записывает в них
тестовые данные, освобождает часть блоков через `free`, снова выделяет память
и проверяет результат.

Сгенерированные `.o`, `.bin` и `.iso` не хранятся в Git.
