# NanoOS

Учебная 32-битная x86 ОС.

## Текущий этап — Phase 13

На этом этапе NanoOS получает первую файловую подсистему: VFS поверх RAMFS.

Это полностью оперативная файловая система. Она существует только до перезагрузки и не использует диск.

Основные возможности:

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
- RAMFS
- дерево каталогов и файлов в памяти
- динамически расширяемое содержимое файлов
- file handles и offsets
- per-process file descriptor table
- файловые syscalls
- shell file manager

## Архитектура файловой системы

Shell / user syscalls
        |
        v
       VFS
        |
        v
      RAMFS
        |
        v
   kernel malloc()

RAMFS хранит дерево:

/
├── directory/
│   └── file.txt
└── another.txt

Каждый vfs_node знает:

- имя;
- тип FILE или DIR;
- размер;
- выделенную ёмкость;
- указатель на данные файла;
- parent;
- first child;
- next sibling;
- количество открытых handles.

Ограничения:

128 nodes
8 shell file descriptors
8 file descriptors на каждый user process
65536 bytes на один файл

## VFS

Поддерживаются:

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

Пути VFS сейчас должны быть абсолютными:

/
 /test
 /test/hello.txt

Shell добавляет к относительным путям свой текущий каталог.

В путях понимаются компоненты:

.
..

Удаление каталога разрешено только когда он пустой.

Открытый файл удалить нельзя.

## Shell file manager

Команды:

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

Пример:

> mkdir test
> cd test
> touch hello.txt
> write hello.txt "Hello NanoOS!"
> ls
[FILE] hello.txt  13 bytes
> cat hello.txt
Hello NanoOS!
> open hello.txt
fd = 0
> read 0
Hello NanoOS!
> close 0

write перезаписывает файл целиком.

open открывает существующий файл для чтения и записи.

read использует текущий offset file handle.

rm удаляет файл или пустой каталог.

## RAMFS

Файлы не существуют после reboot:

boot
  ↓
vfs_init()
  ↓
создаётся новый /
  ↓
старые файлы отсутствуют

Это намеренно.

Сейчас задача RAMFS — дать NanoOS нормальную абстракцию файловой системы до появления настоящего диска.

Позже RAMFS можно заменить другим backend, не меняя VFS.

## File descriptors и syscalls

В каждом user process есть собственная таблица:

fd 0
fd 1
...
fd 7

Она хранит указатели на kernel-side vfs_file.

Добавлены syscalls:

| ID | Назначение |
|----|------------|
| 0 | SYS_EXIT |
| 1 | SYS_WRITE — вывод в терминал |
| 2 | SYS_GETPID |
| 3 | SYS_YIELD |
| 4 | SYS_SBRK |
| 5 | SYS_OPEN |
| 6 | SYS_FILE_READ |
| 7 | SYS_FILE_WRITE |
| 8 | SYS_CLOSE |

SYS_OPEN получает пользовательский указатель на абсолютный путь и флаги.

SYS_FILE_READ безопасно копирует данные из VFS через kernel buffer в user memory.

SYS_FILE_WRITE сначала безопасно читает user buffer через paging API, затем передаёт его VFS.

Размер одной операции файлового syscall ограничен 4096 байтами.

Команда usertest теперь дополнительно проверяет файлы /worker1.txt и /worker2.txt, созданные самими Ring 3 процессами через SYS_OPEN, SYS_FILE_WRITE, SYS_FILE_READ и SYS_CLOSE. Эти файлы остаются в RAMFS после завершения процессов.

Добавлена операция:

paging_read_user_memory()

Она симметрична существующей записи в user memory и нужна для безопасной передачи данных из Ring 3 в kernel.

## Проверка

Для внутреннего теста:

> fstest
fstest: PASS (RAMFS mkdir/create/open/write/read/close)

Тест проверяет полный путь:

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

Для ручной проверки:

> mkdir test
> cd test
> touch hello.txt
> write hello.txt "Hello NanoOS!"
> ls
> cat hello.txt
> open hello.txt
> read 0
> close 0
> cd ..
> rm test/hello.txt
> rm test

## Что пока намеренно не реализовано

У NanoOS всё ещё нет:

- дискового драйвера;
- persistent filesystem;
- программ, загружаемых с диска;
- fork/exec;
- IPC;
- blocked/sleeping states;
- полноценного user malloc/free;
- динамического линкера;
- shared libraries;
- настоящего terminal device;
- графической подсистемы.

## Сборка в Ubuntu

make clean
make
make iso
make run

Для проверки только kernel image:

make check

Сгенерированные .o, .elf, .bin и .iso не хранятся в Git.

## Следующая логическая ступень

Архитектура теперь выглядит так:

VFS
 |
 +-- RAMFS
 |
 +-- future disk filesystem

Следующая крупная задача — драйвер блочного устройства и persistent filesystem, который сможет сохранять файлы после перезагрузки.

После этого становится реалистичным путь:

/bin/test.elf
      ↓
filesystem
      ↓
VFS
      ↓
ELF loader
      ↓
new process
      ↓
Ring 3

То есть Phase 13 — фундамент для настоящего хранения программ, а не просто демонстрация kernel API.
