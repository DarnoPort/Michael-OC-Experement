#include "memory.h"
#include "paging.h"
#include "syscalls.h"
#include "process.h"
#include "vfs.h"
#include "shell.h"
#include "terminal.h"

// NanoOS Phase 14: ATA PIO + persistent DiskFS.

// -----------------------------------------------------------------------------
// 1. Работа с портами
// -----------------------------------------------------------------------------

static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ __volatile__("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(unsigned short port, unsigned char data) {
    __asm__ __volatile__("outb %0, %1" : : "a"(data), "Nd"(port));
}

// -----------------------------------------------------------------------------
// 2. Terminal output helpers are implemented in terminal.c.
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// 3. Структуры IDT
// -----------------------------------------------------------------------------

struct idt_entry {
    unsigned short base_lo;
    unsigned short sel;
    unsigned char always0;
    unsigned char flags;
    unsigned short base_hi;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

extern void load_idt(unsigned int);
extern void keyboard_handler_asm(void);
extern void timer_handler_asm(void);
extern void syscall_handler_asm(void);

#define DECLARE_ISR(n) extern void isr##n();
DECLARE_ISR(0) DECLARE_ISR(1) DECLARE_ISR(2) DECLARE_ISR(3)
DECLARE_ISR(4) DECLARE_ISR(5) DECLARE_ISR(6) DECLARE_ISR(7)
DECLARE_ISR(8) DECLARE_ISR(9) DECLARE_ISR(10) DECLARE_ISR(11)
DECLARE_ISR(12) DECLARE_ISR(13) DECLARE_ISR(14) DECLARE_ISR(15)
DECLARE_ISR(16) DECLARE_ISR(17) DECLARE_ISR(18) DECLARE_ISR(19)
DECLARE_ISR(20) DECLARE_ISR(21) DECLARE_ISR(22) DECLARE_ISR(23)
DECLARE_ISR(24) DECLARE_ISR(25) DECLARE_ISR(26) DECLARE_ISR(27)
DECLARE_ISR(28) DECLARE_ISR(29) DECLARE_ISR(30) DECLARE_ISR(31)
#undef DECLARE_ISR

typedef void (*interrupt_stub_t)(void);

static const interrupt_stub_t exception_stubs[32] = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
    isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
};

void idt_set_gate(unsigned char num, unsigned int base, unsigned short sel, unsigned char flags) {
    idt[num].base_lo = base & 0xFFFF;
    idt[num].base_hi = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

void exception_handler_c(unsigned int vector, unsigned int error_code) {
    __asm__ __volatile__("cli");

    print_string("\n\n*** KERNEL PANIC ***\n", 0x4F);
    print_string("Exception: ", 0x4F);
    print_uint(vector, 0x4F);

    print_string("\nError code: ", 0x4F);
    print_hex32(error_code, 0x4F);

    if (vector == 14) {
        unsigned int fault_address;

        __asm__ __volatile__(
            "mov %%cr2, %0"
            : "=r"(fault_address)
        );

        print_string("\nPage fault address: ", 0x4F);
        print_hex32(fault_address, 0x4F);

        print_string("\nReason: ", 0x4F);
        print_string(
            (error_code & 1U) ? "protection violation" : "non-present page",
            0x4F
        );

        print_string("\nAccess: ", 0x4F);
        print_string(
            (error_code & 2U) ? "write" : "read",
            0x4F
        );

        print_string("\nMode: ", 0x4F);
        print_string(
            (error_code & 4U) ? "user" : "kernel",
            0x4F
        );

        print_string("\n", 0x4F);
    }

    print_string("\nSystem halted.\n", 0x4F);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

// -----------------------------------------------------------------------------
// 4. PIC 8259A
// -----------------------------------------------------------------------------

void init_pic(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    // IRQ0 (timer) and IRQ1 (keyboard) enabled.
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
}

// -----------------------------------------------------------------------------
// 5. PS/2 keyboard
// -----------------------------------------------------------------------------

void keyboard_handler_c(void) {
    unsigned char scancode = inb(0x60);
    terminal_keyboard_scancode(scancode);

    // EOI is sent by keyboard_handler_asm exactly once.
}

// -----------------------------------------------------------------------------
// 6. PIT timer
// -----------------------------------------------------------------------------

volatile unsigned int timer_ticks = 0;

void timer_handler_c(void) {
    timer_ticks++;
}

void init_pit(unsigned int frequency) {
    unsigned int divisor = 1193182 / frequency;

    if (divisor < 1) {
        divisor = 1;
    }

    if (divisor > 65535) {
        divisor = 65535;
    }

    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

// -----------------------------------------------------------------------------
// 7. Multiboot memory information
// -----------------------------------------------------------------------------

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_INFO_MEMORY 0x00000001
#define MULTIBOOT_INFO_MMAP 0x00000040

struct multiboot_info {
    unsigned int flags;
    unsigned int mem_lower;
    unsigned int mem_upper;
    unsigned int boot_device;
    unsigned int cmdline;
    unsigned int mods_count;
    unsigned int mods_addr;
    unsigned int syms[4];
    unsigned int mmap_length;
    unsigned int mmap_addr;
} __attribute__((packed));

struct multiboot_mmap_entry {
    unsigned int size;
    unsigned int base_low;
    unsigned int base_high;
    unsigned int length_low;
    unsigned int length_high;
    unsigned int type;
} __attribute__((packed));

unsigned int multiboot_magic = 0;
unsigned int multiboot_info_addr = 0;

static const char* mmap_type_name(unsigned int type) {
    switch (type) {
        case 1: return "available";
        case 2: return "reserved";
        case 3: return "ACPI reclaimable";
        case 4: return "ACPI NVS";
        case 5: return "bad RAM";
        default: return "unknown";
    }
}

void show_meminfo(void) {
    print_string("Multiboot magic: ", 0x0E);
    print_hex32(multiboot_magic, 0x0F);
    print_char('\n', 0x07);

    if (multiboot_magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        print_string("ERROR: invalid Multiboot magic.\n", 0x0C);
        return;
    }

    if (multiboot_info_addr == 0) {
        print_string("ERROR: Multiboot info pointer is null.\n", 0x0C);
        return;
    }

    struct multiboot_info* info = (struct multiboot_info*)multiboot_info_addr;

    print_string("Info flags: ", 0x0E);
    print_hex32(info->flags, 0x0F);
    print_char('\n', 0x07);

    if (info->flags & MULTIBOOT_INFO_MEMORY) {
        print_string("Lower memory: ", 0x0E);
        print_uint(info->mem_lower, 0x0F);
        print_string(" KB\nUpper memory: ", 0x0E);
        print_uint(info->mem_upper, 0x0F);
        print_string(" KB\n", 0x0E);
    } else {
        print_string("Basic memory information is unavailable.\n", 0x0C);
    }

    if (!(info->flags & MULTIBOOT_INFO_MMAP)) {
        print_string("Memory map is unavailable.\n", 0x0C);
        return;
    }

    print_string("Memory map:\n", 0x0A);

    unsigned int current = info->mmap_addr;
    unsigned int end = info->mmap_addr + info->mmap_length;
    int index = 0;

    if (end < current) {
        print_string("ERROR: memory map address overflow.\n", 0x0C);
        return;
    }

    while (current < end) {
        struct multiboot_mmap_entry* entry =
            (struct multiboot_mmap_entry*)current;
        unsigned int next = current + entry->size + 4;

        if (entry->size < 20 || next < current || next > end) {
            print_string("ERROR: malformed memory map entry.\n", 0x0C);
            return;
        }

        print_char('#', 0x07);
        print_uint((unsigned int)index, 0x0F);
        print_string(": ", 0x07);
        print_string(mmap_type_name(entry->type), 0x0E);
        print_string(" base=", 0x07);
        print_hex64(entry->base_high, entry->base_low, 0x0F);
        print_string(" len=", 0x07);
        print_hex64(entry->length_high, entry->length_low, 0x0F);
        print_char('\n', 0x07);

        current = next;
        index++;

        if (index >= 64) {
            print_string("Memory map truncated after 64 entries.\n", 0x0C);
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// 8. Shell helpers
// -----------------------------------------------------------------------------

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }

    return *(unsigned const char*)s1 - *(unsigned const char*)s2;
}

void clear_screen(void) {
    terminal_clear();
}

static void print_uptime(void) {
    unsigned int ticks = timer_ticks;
    unsigned int seconds = ticks / 100;
    unsigned int remainder = ticks % 100;

    print_string("Uptime: ", 0x0E);
    print_uint(seconds, 0x0F);
    print_char('.', 0x0F);

    if (remainder < 10) {
        print_char('0', 0x0F);
    }

    print_uint(remainder, 0x0F);
    print_string(" s\n", 0x0E);
}

// -----------------------------------------------------------------------------
// 9. Главный цикл
// -----------------------------------------------------------------------------

void kernel_main(unsigned int magic, unsigned int info_addr) {
    __asm__ __volatile__("cli");

    terminal_init();

    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (unsigned int)&idt;

    for (int i = 0; i < 32; i++) {
        idt_set_gate((unsigned char)i, (unsigned int)exception_stubs[i], 0x08, 0x8E);
    }

    idt_set_gate(32, (unsigned int)timer_handler_asm, 0x08, 0x8E);
    idt_set_gate(33, (unsigned int)keyboard_handler_asm, 0x08, 0x8E);
    idt_set_gate(128, (unsigned int)syscall_handler_asm, 0x08, 0xEE);

    load_idt((unsigned int)&idtp);
    init_pic();
    init_pit(100);

    print_string("=== Michael OS 0.15: Text Terminal ===\n", 0x0A);

    if (!memory_init(magic, info_addr)) {
        print_string("WARNING: physical memory manager initialization failed.\n", 0x0C);
    } else {
        print_string("Physical page allocator initialized.\n", 0x0E);

        if (!paging_init()) {
            print_string("WARNING: paging initialization failed.\n", 0x0C);
        } else {
            print_string("Paging enabled.\n", 0x0E);
            print_string("Kernel virtual memory enabled.\n", 0x0E);
            print_string("Kernel heap now uses virtual pages.\n", 0x0E);

            if (!vfs_init()) {
                print_string(
                    "WARNING: DiskFS/VFS initialization failed.\n",
                    0x0C
                );
            } else {
                print_string(
                    "VFS + DiskFS initialized.\n",
                    0x0E
                );
            }
        }
    }

    shell_init();

    if (!syscall_init()) {
        print_string("WARNING: Ring 3/syscall initialization failed.\n", 0x0C);
    } else {
        print_string("Ring 3 and syscall interface initialized.\n", 0x0E);
    }

    print_string("Type 'help' for commands.\n", 0x0E);
    terminal_prompt();

    __asm__ __volatile__("sti");

    while (1) {
        if (terminal_command_ready()) {
            __asm__ __volatile__("cli");

            const char* command =
                terminal_get_command();

            if (strcmp(command, "help") == 0) {
                print_string(
                    "Commands: help, ver, history, clear, cls, uptime, ticks, meminfo, physinfo, memtest, paging, vmtest, pfault, ps, usertest, diskinfo, pwd, ls, dir, cd, mkdir, touch, write, cat, type, open, read, close, rm, fstest\n",
                    0x0E
                );
            } else if (strcmp(cmd_buffer, "uptime") == 0) {
                print_uptime();
            } else if (strcmp(cmd_buffer, "ticks") == 0) {
                print_string("Timer ticks: ", 0x0E);
                print_uint(timer_ticks, 0x0F);
                print_char('\n', 0x07);
            } else if (strcmp(cmd_buffer, "meminfo") == 0) {
                memory_print_info();
            } else if (strcmp(cmd_buffer, "physinfo") == 0) {
                memory_print_stats();
            } else if (strcmp(cmd_buffer, "memtest") == 0) {
                memory_test();
            } else if (strcmp(cmd_buffer, "paging") == 0) {
                paging_print_info();
            } else if (strcmp(cmd_buffer, "vmtest") == 0) {
                paging_test();
            } else if (strcmp(cmd_buffer, "pfault") == 0) {
                print_string("Triggering test page fault...\n", 0x0C);
                paging_trigger_page_fault();
            } else if (strcmp(cmd_buffer, "ps") == 0) {
                scheduler_print_processes();
            } else if (strcmp(cmd_buffer, "usertest") == 0) {
                syscall_run_test();
            } else if (strcmp(cmd_buffer, "clear") == 0) {
                clear_screen();
            } else if (shell_handle_command(cmd_buffer)) {
                // Filesystem/shell command was handled by shell.c.
            } else if (strcmp(cmd_buffer, "sleep") == 0) {
                print_string("sleep is not implemented yet.\n", 0x09);
            } else if (terminal_command_length() > 0U) {
                print_string("Unknown command: ", 0x0C);
                print_string(command, 0x0C);
                print_char('\n', 0x07);
            }

            terminal_command_consumed();

            __asm__ __volatile__("sti");
        }

        __asm__ __volatile__("hlt");
    }
}
