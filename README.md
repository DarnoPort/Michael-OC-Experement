# Michael OS

Учебная 32-битная x86 ОС.

## Текущий этап — Phase 21

Phase 21 начинает плавное приближение интерфейса Michael OS к классическому DOS-подобному терминалу. Первый шаг — структурированное приглашение командной строки, зависящее от текущего каталога.

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
- ELF32 loader для встроенных и VFS-загруженных ELF32 image
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
- запуск ELF32 программ с DiskFS через команду `run <path> [args...]`
- `SYS_EXEC` для замены текущего user process новым ELF image
- стартовый `argc`/`argv` для ELF-программ
- повторное использование PID при `exec()`
- клавиатурные раскладки EN/RU
- Shift и CapsLock
- Alt+Shift для переключения раскладки
- Ctrl+C / Ctrl+L / Ctrl+U / Ctrl+A / Ctrl+E
- встроенный CP866 Cyrillic VGA font

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

Phase 16 реализует эту ступень: ELF можно хранить на DiskFS и запускать через shell без встраивания самой программы в kernel image.


## Phase 16: Executable Programs

Phase 16 делает важный архитектурный переход: программа теперь может быть обычным файлом на DiskFS.

Добавлено:

- ELF-loader принимает произвольный буфер с ELF32 image;
- сохранён совместимый путь для старого встроенного `usertest`;
- `process_create_from_image()` создаёт Ring 3 процесс из переданного ELF;
- `process_run_image()` запускает пользовательский процесс и возвращает управление shell после его завершения;
- команда `install-demo` устанавливает встроенную демонстрационную программу как `/bin/demo.elf`;
- команда `run <path>` читает ELF через VFS, создаёт процесс и запускает его в Ring 3;
- executable больше не обязан быть частью kernel image.

Пример:

```text
> install-demo
Installed /bin/demo.elf (... bytes).

> ls /bin
[FILE] demo.elf  ... bytes

> run /bin/demo.elf
[run] loading /bin/demo.elf into Ring 3...
...
[run] process 1 exited.
```

Теперь цепочка запуска выглядит так:

```text
Shell
  |
  v
VFS
  |
  v
DiskFS
  |
  v
ELF file in memory
  |
  v
ELF loader
  |
  v
Process / CR3
  |
  v
Ring 3
```

Это ещё не полноценный Unix `exec()`: shell пока остаётся частью kernel, а аргументы командной строки и окружение процесса ещё не передаются.

Ограничение текущего этапа: DiskFS хранит максимум 64 KiB на файл, поэтому текущие ELF-программы должны укладываться в это ограничение.

## Phase 18: Process Arguments (`argc` / `argv`)

Phase 18 превращает запуск ELF из просто «запустить файл» в нормальный запуск процесса с параметрами.

Добавлено:

- `PROCESS_ARG_MAX` — максимум 8 аргументов вместе с `argv[0]`;
- `PROCESS_ARG_MAX_LEN` — максимум 127 символов на один аргумент;
- отдельный `initial_user_esp`, чтобы не путать вершину выделенной страницы стека и начальный `ESP`;
- `argc` и `argv[]` формируются ядром непосредственно на user stack;
- `argv[argc] == NULL`;
- третий стартовый параметр `envp` пока передаётся как `NULL`;
- `run <path> [args...]` передаёт аргументы процессу;
- двойные кавычки позволяют сделать один аргумент из нескольких слов;
- встроенный `/bin/args-test.elf` показывает фактические `argc` и `argv`.

Структура стартового user stack:

```text
ESP -> argc
       argv
       envp = NULL

argv -> argv[0]
        argv[1]
        ...
        argv[argc] = NULL

        строка argv[0]
        строка argv[1]
        ...
```

Пример:

```text
> install-args-test
> run /bin/args-test.elf alpha beta "hello world"

[run] loading /bin/args-test.elf into Ring 3... with 3 arg(s)
=== argv test ===
argc=4
argv[0]=/bin/args-test.elf
argv[1]=alpha
argv[2]=beta
argv[3]=hello world
```

Это ещё не полноценный Unix process environment: `envp` не реализован, а shell остаётся частью ядра.

## Phase 21: Terminal Foundation — DOS-style Prompt

Phase 21 намеренно небольшая: логика shell, VFS, процессов и syscalls не меняется. Вместо этого терминал получает отдельное понятие текущего текста приглашения.

Добавлено:

- `terminal_set_prompt()` — терминал больше не зашит на строку `> `;
- shell автоматически формирует prompt из текущего `cwd`;
- корневой каталог отображается как `C:\>`;
- пример подкаталога: `C:\TEST>`;
- при `cd` prompt сразу меняется;
- `Ctrl+C` и `Ctrl+L` продолжают использовать актуальный prompt;
- внутри VFS по-прежнему используются пути `/...`, а `C:\...>` пока является только DOS-подобным представлением для интерфейса;
- версия проекта переведена на `0.21` через единый `version.mk` из Phase 20.

Пример:

```text
=== Michael OS 0.21: Terminal Foundation ===

C:\>
C:\> mkdir test
C:\> cd test
C:\TEST> write hello.txt "Hello Michael OS!"
C:\TEST> dir
[FILE] hello.txt  17 bytes
C:\TEST> cd ..
C:\>
```

Это специально не полноценная DOS-совместимость. В этой фазе мы закладываем только разделение между shell-состоянием (`cwd`) и визуальным представлением prompt. Позже на этот слой можно без перелома архитектуры добавить более DOS-подобные команды, работу с дисками и другие элементы интерфейса.

## Phase 20: Jubilee — Unified Versioning

Phase 20 не добавляет ещё одну разрозненную цифру версии. Вместо этого версия Michael OS хранится в одном месте — version.mk.

Добавлено:

- version.mk как единый источник версии;
- автоматическая генерация build/version.h для kernel/shell;
- автоматическая генерация build/grub.cfg из grub.cfg.in;
- стартовый экран использует MICHAEL_OS_VERSION_STRING;
- команда ver использует ту же версию;
- GRUB показывает ту же версию и помечает юбилейную сборку как Jubilee;
- старый статический grub.cfg больше не используется.

Теперь для следующей версии достаточно изменить одну строку:

```text
MICHAEL_OS_VERSION := 0.21
```

в version.mk, после чего все три видимых места обновятся при обычной сборке.

Проверка:

```text
> ver
Michael OS 0.20 - 32-bit x86 experimental OS.
```

При старте:

```text
=== Michael OS 0.20: Jubilee ===
```

А в меню GRUB:

```text
Michael OS 0.20 (Jubilee)
```

0.15, 0.17 и 0.18, встречающиеся ниже в README, относятся к историческим описаниям соответствующих фаз и намеренно не переписываются.

## Phase 19: Exec Arguments

Phase 19 завершает связку `run` → `argc/argv` → `exec()`.

Добавлено:

- новый вариант `process_exec_image_with_args()`, который строит новый user stack с переданным `argc`/`argv`;
- `SYS_EXEC` теперь принимает ABI: `EBX = path`, `ECX = argv`, `EDX = argc`;
- ядро копирует массив указателей и строки `argv` из старого address space до переключения CR3;
- проверяется `argv[argc] == NULL`;
- PID, kernel stack и открытые file descriptors сохраняются при `exec()`;
- `user_exec.asm` теперь запускает `/bin/args-test.elf` и передаёт ему аргументы;
- существующий `args-test` тем самым показывает параметры уже после замены процесса, а не только после `run`.

Проверка:

```text
> install-args-test
> install-exec-test
> run /bin/exec-test.elf
=== argv test ===
argc=3
argv[0]=/bin/args-test.elf
argv[1]=exec
argv[2]=phase19
```

Важный результат: после успешного `exec()` новый ELF получает нормальный стартовый стек с тем же процессом, но с новым `argv`. Это уже позволяет строить следующий слой — пользовательские runtime-библиотеки и настоящую модель запуска программ.

## Phase 17: Exec and International Keyboard Input

Phase 17 делает следующий архитектурный шаг: процесс теперь может заменить собственный user-space образ через настоящий `SYS_EXEC`, а терминал получает полноценный двуязычный ввод.

Добавлено:

- `SYS_EXEC` (ID 9);
- загрузка ELF-файла из VFS внутри syscall слоя;
- `process_exec_image()` сохраняет PID, kernel stack и открытые file descriptors, но заменяет CR3, user stack, heap и entry point;
- отдельный путь возврата из `syscall_handler_asm` непосредственно в новый execution context;
- `user_exec.asm` как демонстрационная программа, вызывающая `SYS_EXEC` для `/bin/demo.elf`;
- команда `install-exec-test`, устанавливающая `/bin/exec-test.elf`;
- Shift и CapsLock;
- EN/RU раскладки;
- Alt+Shift для переключения раскладки;
- Ctrl+C отменяет текущую команду и выводит `^C`;
- Ctrl+L очищает экран;
- Ctrl+U очищает текущую командную строку;
- Ctrl+A / Ctrl+E перемещают курсор в начало/конец строки;
- 8x16 CP866 glyphs для кириллицы загружаются непосредственно в VGA font plane, сохраняя остальные ROM glyphs.

Проверка `exec()`:

```text
> install-demo
> install-exec-test
> run /bin/exec-test.elf
```

`exec-test.elf` вызывает `SYS_EXEC`, после чего тот же процесс получает новый ELF-образ `/bin/demo.elf`. PID не меняется, а старый user address space уничтожается после переключения на новый.

Проверка клавиатуры:

```text
> layout ru
Привет Michael OS
> layout en
Hello Michael OS
```

В текстовом VGA режиме терминал использует однобайтные CP866-коды для кириллицы. Это сознательно не UTF-8: до графического framebuffer терминалу выгоднее сохранить компактную однобайтную модель.



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
install-demo
install-exec-test
run <path>

layout [en|ru]

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
| 9 | SYS_EXEC |

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

- fork;

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

Теперь запуск программ с диска уже реализован через `run`:

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

Phase 16 превращает DiskFS из хранилища данных в источник исполняемых программ.

Следующие ступени можно посвятить:

- `fork()`;
- расширению `exec()` для передачи `argv` / `envp`;
- stdin / stdout / stderr как настоящим файловым дескрипторам;
- blocked processes и `sleep()`;
- user-space runtime / libc-подобной библиотеке;
- framebuffer и графической подсистеме.

## Toolchain

Проект рассчитан на Ubuntu/WSL с:

- gcc multilib;
- nasm;
- binutils/ld;
- grub-file;
- grub-mkrescue;
- qemu-system-i386.

Сгенерированные .o, .elf, .bin, .iso и michaelos.disk не хранятся в Git.
