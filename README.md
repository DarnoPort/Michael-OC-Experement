# NanoOS

Учебная 32-битная x86 ОС.

Текущий этап:
- GRUB Multiboot
- собственная flat GDT
- IDT на 256 записей
- PIC 8259A, клавиатура через IRQ1
- VGA text mode 80x25
- shell: help, clear, sleep

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

Сгенерированные .o, .bin и .iso специально не хранятся в Git: они должны
получаться из исходников.