# NanoOS

Учебная 32-битная x86 ОС.

## Текущий этап — Phase 10

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
- shell: help, clear, uptime, ticks, meminfo, physinfo, memtest, paging, vmtest, pfault, usertest

## Архитектура Phase 10

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

У NanoOS ещё нет процесса как самостоятельного объекта, scheduler или отдельных address spaces на каждый процесс.

Phase 10 только создаёт основу:
- Ring 3
- TSS
- syscalls
- user memory
- безопасный переход user -> kernel -> user/kernel return

Следующая крупная стадия может использовать этот фундамент для настоящих процессов и вытесняющей многозадачности.

## Сборка в Ubuntu

```bash
make clean
make
make iso
make run
```

Сгенерированные `.o`, `.bin` и `.iso` не хранятся в Git.