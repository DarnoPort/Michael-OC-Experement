# NanoOS

Учебная 32-битная x86 ОС.

## Текущий этап — Phase 11

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
- virtual page allocator
- Page Fault с расшифровкой адреса и error code
- null page protection
- CR0.WP
- user VM область 0x80000000–0x803FFFFF
- user code и user stack с отдельными правами страниц
- GDT-сегменты Ring 3
- 32-битный TSS с отдельным kernel stack при входе из Ring 3
- DPL3 system call gate на interrupt 0x80
- SYS_WRITE и SYS_EXIT
- проверка user pointers перед SYS_WRITE
- возврат из демонстрационной user-программы обратно в kernel shell
- PCB и PID для пользовательских процессов
- preemptive round-robin scheduler на IRQ0 (100 Hz)
- отдельный kernel stack на каждый процесс
- отдельные физические code/stack страницы на каждый процесс
- переключение текущего user address mapping при context switch
- SYS_GETPID и SYS_YIELD
- shell: help, clear, uptime, ticks, meminfo, physinfo, memtest, paging, vmtest, pfault, ps, usertest

## Архитектура Phase 11

NanoOS теперь имеет реальную границу привилегий:

```
Ring 3
  |
  | int 0x80
  v
TSS.esp0
  |
  v
Ring 0
  |
  +-- syscall dispatcher
  +-- kernel heap
  +-- physical memory
  +-- virtual memory
```

Пользовательский тестовый код запускается в отдельной user VM-области:

```
0x80000000  user code
0x80001000  user stack
0x80002000  end of test stack
```

Kernel address space остаётся недоступным из Ring 3, потому что соответствующие PDE/PTE не имеют PAGE_USER.

SYS_WRITE принимает:
- EAX = 1
- EBX = user buffer
- ECX = length

Ядро проверяет весь диапазон пользовательской памяти перед чтением.

SYS_EXIT завершает демонстрационный процесс и возвращает управление исходному kernel stack, после чего usertest продолжает выполнение в Ring 0.

## Команды Phase 10

```text
usertest
paging
vmtest
pfault
memtest
```

Ожидаемый результат usertest:

```text
> usertest
Entering Ring 3...
Hello from Ring 3! System call works.
[syscall] user program exited.
Returned to kernel from Ring 3.
>
```

## Что пока намеренно не реализовано

Phase 11 уже содержит процессы и вытесняющее переключение контекста, но address space пока общий для всех процессов.

При каждом переключении scheduler меняет физические страницы, отображённые в:
- 0x80000000 — user code
- 0x80001000 — user stack

Поэтому неактивный процесс хранит свои физические страницы отдельно, а активный получает их через фиксированное user virtual address space.

Пока ещё нет:
- отдельного page directory для каждого процесса;
- ELF loader;
- файловой системы;
- fork/exec;
- IPC;
- sleeping/blocked process states;
- настоящего user heap;
- полноценного процесса shell.

Следующая логичная стадия — отдельные address spaces, динамический loader и блокирующие состояния процессов.

## Сборка в Ubuntu

```bash
make clean
make
make iso
make run
```

Сгенерированные `.o`, `.bin` и `.iso` не хранятся в Git.

## Демонстрация Phase 11

Команда `usertest` создаёт два пользовательских процесса. Каждый процесс получает:
- PID;
- собственную физическую страницу кода;
- собственную физическую страницу стека;
- собственную страницу kernel stack;
- сохранённый CPU context для scheduler.

Таймер 100 Hz выполняет round-robin переключение между Ring 3 процессами. Тестовая программа несколько раз печатает свой PID и выполняет намеренную busy-loop нагрузку, чтобы таймер успевал вытеснять её.

Команда `ps` показывает таблицу созданных процессов.

## Ограничение Phase 11

Это ещё не полная изоляция address space уровня современных ОС. User virtual addresses у процессов одинаковые, а page table физически общая. Изоляция достигается тем, что scheduler переключает отображённые физические code/stack страницы. Отдельные page directories для процессов являются отдельной следующей стадией.
