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
- shell: help, clear, uptime, ticks, meminfo
- получение карты памяти от Multiboot

## Сборка

Установите NASM, GCC multilib, binutils, GRUB и QEMU.

```bash
make clean
make
make iso
make run
```

Проверка Multiboot:

```bash
make check
```

После запуска полезно выполнить:

```text
meminfo
```

Команда показывает информацию о памяти, которую GRUB передал ядру, включая
таблицу доступных и зарезервированных областей.

Сгенерированные .o, .bin и .iso специально не хранятся в Git: они должны
получаться из исходников.
