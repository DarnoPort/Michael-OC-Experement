# NanoOS

Учебная 32-битная x86 ОС.

## Текущий этап — Phase 12

На этом этапе NanoOS переходит от нескольких user-mode задач в одном общем адресном пространстве к отдельным address spaces.

Основные возможности:

- GRUB Multiboot
- собственная flat GDT
- IDT на 256 записей
- CPU exception handlers
- PIC 8259A
- PIT timer 100 Hz
- клавиатура через IRQ1
- VGA text mode 80x25 со скроллингом
- физический allocator страниц 4 КБ на основе Multiboot memory map
- kernel heap с malloc/free
- 32-битный paging без PAE
- kernel virtual-memory область 0xC0000000–0xC3FFFFFF
- Page Fault с расшифровкой адреса и error code
- null page protection
- CR0.WP
- Ring 3
- TSS с отдельным kernel stack при входе из Ring 3
- DPL3 system call gate на interrupt 0x80
- scheduler и preemptive round-robin на IRQ0
- PID и PCB для процессов
- отдельный page directory (CR3) для каждого процесса
- отдельная user page table для каждого процесса
- общий kernel address space, присутствующий во всех процессах
- отдельный kernel stack для каждого процесса
- ELF32 loader из встроенного user ELF image
- PT_LOAD загрузка, BSS zero-fill и проверка границ
- page permissions из ELF PF_* после загрузки
- SYS_WRITE
- SYS_GETPID
- SYS_YIELD
- SYS_SBRK
- SYS_EXIT
- user heap в диапазоне 0x80100000–0x803EFFFF
- shell: help, clear, uptime, ticks, meminfo, physinfo, memtest, paging, vmtest, pfault, ps, usertest

## Архитектура

Каждый пользовательский процесс имеет собственный CR3:

```
Process A
  CR3 A
   ├── kernel mappings
   └── user page table A
          ├── ELF code/data
          ├── heap
          └── stack

Process B
  CR3 B
   ├── kernel mappings
   └── user page table B
          ├── ELF code/data
          ├── heap
          └── stack
```

Kernel VM остаётся общим:

```
0xC0000000 - 0xC3FFFFFF
```

User VM:

```
0x80000000 - 0x803FFFFF
```

В одном 4 MiB user VM используется одна page table. Это специально сохраняет реализацию достаточно простой для учебной ОС, но уже даёт настоящую изоляцию адресного пространства между процессами.

## ELF loader

Пользовательская программа сначала собирается как отдельный ELF32:

```
user_program.asm
      ↓ NASM
user_program_raw.o
      ↓ ld + user.ld
user_program.elf
      ↓ incbin
kernel image
```

При создании процесса loader:

1. проверяет ELF magic/class/endianness/type/machine;
2. проверяет таблицу program headers и границы файла;
3. находит PT_LOAD сегменты;
4. проверяет границы user VM;
5. выделяет физические страницы;
6. копирует p_filesz;
7. зануляет p_memsz - p_filesz;
8. выставляет конечные права страниц по PF_*;
9. возвращает entry point процесса.

Сейчас для надёжности загрузчик не допускает перекрывающиеся PT_LOAD страницы.

## User heap

В процессе имеется простой program break:

```
USER_HEAP_BASE = 0x80100000
USER_HEAP_END  = 0x803F0000
```

`SYS_SBRK` принимает увеличение break в EBX и возвращает старое значение break в EAX.

Пока поддерживается только рост heap. Это намеренно минимальный аналог традиционного `sbrk`, достаточный как фундамент для будущего malloc в user space.

## Демонстрация

Команда:

```
> usertest
```

создаёт два процесса.

Каждый получает:

- собственный PID;
- собственный CR3;
- собственный kernel stack;
- собственные физические user pages;
- собственный heap.

Тестовая user-программа:

- получает PID через `SYS_GETPID`;
- резервирует страницу через `SYS_SBRK`;
- записывает PID в свой heap;
- вызывает `SYS_WRITE`;
- создаёт CPU-нагрузку;
- вызывает `SYS_YIELD`;
- завершается через `SYS_EXIT`.

Ожидаемый смысл вывода — повторяющиеся PID двух процессов, например:

```
1
2
1
2
...
[syscall] user process exited.
[syscall] user process exited.
All user processes have returned to the kernel.
>
```

Точный порядок зависит от работы timer scheduler.

Команда:

```
> ps
```

показывает PID, состояние и CR3 процессов.

## Что пока намеренно не реализовано

У NanoOS всё ещё нет:

- файловой системы;
- дискового драйвера;
- ELF-программ, загружаемых с диска;
- fork/exec;
- IPC;
- blocked/sleeping states;
- полноценного user malloc/free;
- динамического линкера;
- shared libraries;
- настоящего terminal device;
- графической подсистемы.

Следующая логичная крупная стадия — файловая подсистема и блочное хранилище.

## Сборка в Ubuntu

```bash
make clean
make
make iso
make run
```

Для проверки только kernel image:

```bash
make check
```

Сгенерированные `.o`, `.elf`, `.bin` и `.iso` не хранятся в Git.
