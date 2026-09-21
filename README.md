# NanoOS

Учебная 32-битная x86 ОС.

## Текущий этап — Phase 9

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
- 4 КБ page tables для low memory
- read-only kernel text/rodata страницы
- null page не отображается
- CR0.WP включён
- отдельная kernel virtual-memory область 0xC0000000–0xC3FFFFFF
- virtual page allocator с отображением виртуальных страниц на физические
- kernel heap работает поверх virtual memory
- Page Fault с расшифровкой адреса и error code
- shell: help, clear, uptime, ticks, meminfo, physinfo, memtest, paging, vmtest, pfault

## Память

Phase 8 дал физический page allocator и kernel heap.

Phase 9 добавляет слой виртуальной памяти:

```
malloc()
   ↓
kernel heap
   ↓
virtual page allocator
   ↓
page tables
   ↓
physical page allocator
   ↓
RAM
```

Поэтому физические страницы, выданные heap, больше не обязаны идти подряд. Виртуальный адрес heap остаётся непрерывным.

Первые 4 МБ физической памяти отображаются через 4 КБ page table. Нулевая страница `0x00000000` намеренно не отображается. Остальная физическая память до 4 ГБ на этапе bootstrap отображается identity mapping через 4 МБ pages. Область `0xC0000000–0xC3FFFFFF` заменена на 4 КБ page tables виртуальной памяти ядра.

Kernel text и rodata отображаются без разрешения записи. После включения `CR0.WP` попытка записи в такую страницу вызывает Page Fault даже в Ring 0.

## Команды Phase 9

```text
paging
vmtest
pfault
```

`paging` показывает состояние CR0/CR3/CR4 и использование VM-области.

`vmtest` выделяет две виртуальные страницы, проверяет перевод виртуальных адресов в физические, чтение/запись, затем освобождает страницы.

`pfault` намеренно обращается к нулевой странице и должен завершить работу ядра с диагностикой Page Fault.

## Сборка в Ubuntu

```bash
make clean
make
make iso
make run
```

Сгенерированные `.o`, `.bin` и `.iso` не хранятся в Git.
