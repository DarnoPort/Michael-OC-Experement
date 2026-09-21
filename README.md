# Michael OS

Учебная 32-битная x86 ОС.

## Текущий этап — Phase 15

На этом этапе Michael OS получает первый полноценный текстовый терминал поверх уже работающих VFS, DiskFS и Ring 3.

Phase 13 давала VFS поверх RAMFS, поэтому файлы существовали только до reboot. Phase 14 сохраняет ту же VFS-интерфейсную часть, но заменяет RAMFS-хранилище на простой дисковый backend DiskFS.

Основная цепочка теперь:

Shell / user syscalls
        |
        v
       VFS
        |
        v
     DiskFS
        |
        v
     ATA PIO
        |
        v
   IDE disk image

После перезагрузки Michael OS дерево каталогов и содержимое файлов восстанавливаются с диска.

## Возможности

- GRUB Multiboot
- flat GDT
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
- общий kernel address space
- отдельный kernel stack для каждого процесса
- ELF32 loader из встроенного user ELF image
- PT_LOAD загрузка, BSS zero-fill и проверка границ
- page permissions из ELF PF_* после загрузки
- user heap через SYS_SBRK
- VFS
- persistent DiskFS
- ATA PIO IDE block driver
- дерево каталогов и файлов на диске
- file handles и offsets
- per-process file descriptor table
- файловые syscalls
- shell file manager

## Phase 15: Text Terminal

Phase 15 отделяет консольный вывод и ввод от kernel.c в отдельный модуль terminal.c.

Это всё ещё не графический интерфейс. Michael OS работает в стандартном VGA text mode 80x25, но теперь терминал ведёт себя как отдельная подсистема, а не просто как поток символов.

Добавлено:

- аппаратный курсор VGA;
- отдельный terminal.c / terminal.h;
- редактирование командной строки;
- Left / Right;
- Home / End;
- Delete / Backspace;
- история из 16 последних команд;
- Up / Down для навигации по истории;
- Tab как четыре пробела;
- команда history;
- команда ver;
- cls как псевдоним clear;
- dir как псевдоним ls;
- type как псевдоним cat.

Командная строка в этой фазе намеренно ограничена одной строкой экрана. Это упрощает редактор и оставляет сложный многострочный ввод на будущее.

Пример:

```text
Michael OS 0.15

> ver
Michael OS 0.15 - 32-bit x86 experimental OS.

> mkdir test
> write test/hello.txt "Hello Michael OS!"
> type test/hello.txt
Hello Michael OS!

> history
  1  ver
  2  mkdir test
  3  write test/hello.txt "Hello Michael OS!"
  4  type test/hello.txt
```

Стрелки Up / Down позволяют вернуть старую команду и отредактировать её до повторного запуска.

Архитектура терминала:

```text
Keyboard IRQ
     |
     v
terminal.c
     |
     +---- VGA text buffer
     |
     +---- command line editor
     |
     +---- command history
     |
     v
Michael OS shell
     |
     v
VFS
     |
     v
DiskFS
     |
     v
ATA PIO
     |
     v
disk image
```

Следующая крупная ступень — загрузка ELF непосредственно из VFS и запуск программ с диска вместо единственного встроенного тестового образа.

## Phase 14: DiskFS

DiskFS — специально маленькая файловая система для Michael OS.

Она не пытается быть FAT/ext2/Unix FS. Её задача — дать ОС настоящий persistent block-storage слой, на котором можно продолжать строить более высокие уровни.

### Диск

Makefile создаёт файл:

michaelos.disk

Размер:

16 MiB

На первом запуске DiskFS видит пустой образ и автоматически форматирует его.

На следующих запусках тот же образ монтируется, поэтому файлы остаются.

Чтобы полностью начать с чистого диска:

make disk-reset

После этого следующий запуск снова создаст новый пустой образ.

### Разметка диска

Sector 0:
superblock

Sectors 1–16:
inode table

Sectors 17–24:
data-sector bitmap

Sector 25+:
file data

Размер сектора:
512 bytes

DiskFS использует LBA28 ATA PIO, чего более чем достаточно для текущего 16 MiB образа.

## Inodes

Всего:

128 inodes

Каждый inode хранит:

- used
- type
- size
- первый сектор данных
- количество секторов
- parent inode
- имя файла/каталога

Каталоги не содержат отдельного списка directory entries. Иерархия восстанавливается через parent inode.

Это сознательно простой дизайн: VFS всё равно строит нормальное дерево объектов в памяти после загрузки.

## Ограничения текущего DiskFS

- максимум 128 inodes;
- максимальный размер одного файла — 64 KiB;
- один файл занимает непрерывный диапазон дисковых секторов;
- максимум 16 MiB используемого дискового образа;
- нет journaling;
- нет fsck/recovery;
- нет прав доступа файлов;
- нет timestamp;
- нет symbolic links;
- нет hard links;
- нет multi-user storage;
- ATA driver сейчас рассчитан на primary IDE master;
- DiskFS использует простое copy-on-write обновление файла.

Последний пункт важен: при записи новый набор секторов сначала выделяется и заполняется, затем inode переключается на новую область. Это снижает риск оставить inode указывать на частично записанные новые данные при ошибке записи.

Цена простоты — возможная фрагментация и дополнительная запись на диск.

## VFS

Публичный VFS API сохранился:

vfs_init()
vfs_lookup()
vfs_mkdir()
vfs_create_file()
vfs_remove()

vfs_open()
vfs_read()
vfs_write()
vfs_seek()
vfs_close()
vfs_truncate()

Shell по-прежнему может использовать относительные пути. Внутри VFS используются абсолютные пути:

/
/test
/test/hello.txt

Поддерживаются:

.
..

VFS кэширует содержимое открытого файла в памяти. Если файл уже есть на диске, его содержимое подгружается при первом обращении.

Запись через vfs_write() сразу сохраняется в DiskFS. Поэтому закрытие файла не является условием сохранения данных.

## Shell

Команды:

help
clear
uptime
ticks
meminfo
physinfo
memtest
paging
vmtest
pfault
ps
usertest
diskinfo

pwd
ls [path]
cd <path>
mkdir <path>
touch <path>
write <path> <text>
cat <path>
open <path>
read <fd> [length]
close <fd>
rm <path>
fstest

### diskinfo

Показывает:

- общее количество секторов DiskFS;
- свободные data sectors;
- занятые inodes.

Пример:

> diskinfo
DiskFS sectors: 32768
Free data sectors: ...
Used inodes: ...

### Пример

> mkdir test
> cd test
> touch hello.txt
> write hello.txt "Hello Michael OS!"
> ls
[FILE] hello.txt  13 bytes
> cat hello.txt
Hello Michael OS!

Теперь можно выйти из QEMU:

Ctrl+C

или закрыть окно QEMU, затем снова:

make run

и проверить:

> cd /test
> cat hello.txt

Файл должен остаться.

### Проверка FS

> fstest

Проверяет:

mkdir
  ↓
create
  ↓
open
  ↓
write
  ↓
seek
  ↓
read
  ↓
close

Теперь этот тест работает уже через DiskFS, а не через RAMFS.

## Ring 3

User-процессы используют:

| ID | Назначение |
|----|------------|
| 0 | SYS_EXIT |
| 1 | SYS_WRITE |
| 2 | SYS_GETPID |
| 3 | SYS_YIELD |
| 4 | SYS_SBRK |
| 5 | SYS_OPEN |
| 6 | SYS_FILE_READ |
| 7 | SYS_FILE_WRITE |
| 8 | SYS_CLOSE |

Команда:

> usertest

создаёт два Ring 3 процесса.

Они:

1. выделяют user heap;
2. получают PID;
3. создают /worker1.txt и /worker2.txt;
4. записывают туда PID;
5. закрывают файлы;
6. снова открывают их;
7. читают данные через SYS_FILE_READ;
8. проверяют содержимое;
9. завершаются.

После завершения процессов файлы остаются на диске.

Поэтому Phase 14 впервые связывает сразу несколько подсистем:

Ring 3
  ↓
syscalls
  ↓
VFS
  ↓
DiskFS
  ↓
ATA PIO
  ↓
диск

## Сборка

Полная сборка:

make clean
make
make iso
make run

Проверка Multiboot kernel image:

make check

Создание диска выполняется автоматически при make run.

Если файл michaelos.disk уже существует, он не перезаписывается.

## Что пока не реализовано

У Michael OS всё ещё нет:

- программ, загружаемых с диска;
- fork/exec;
- IPC;
- blocked/sleeping states;
- полноценного user malloc/free;
- динамического линкера;
- shared libraries;
- настоящего terminal device;
- device filesystem;
- pipes;
- нормальной файловой модели Unix;
- графической подсистемы.

## Дальнейшая архитектура

Теперь путь к запуску программ с диска становится реальным:

/bin/test.elf
      ↓
DiskFS
      ↓
VFS
      ↓
ELF loader
      ↓
new process
      ↓
Ring 3

Это уже следующая логическая большая ступень.

Phase 14 прежде всего добавляет физическое хранение данных.

После неё можно отдельно заниматься:

- загрузкой ELF непосредственно из VFS;
- exec();
- созданием процессов из файлов;
- нормальным shell для запуска программ;
- более серьёзной файловой системой;
- устройствами как файлами;
- blocked processes и sleep();
- развитием текстового terminal UI.

## Toolchain

Проект рассчитан на Ubuntu/WSL с:

- gcc multilib;
- nasm;
- binutils/ld;
- grub-file;
- grub-mkrescue;
- qemu-system-i386.

Сгенерированные .o, .elf, .bin, .iso и michaelos.disk не хранятся в Git.
