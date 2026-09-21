// NanoOS Phase 8: Physical memory + kernel heap + timer + keyboard.

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
// 2. Видеотерминал
// -----------------------------------------------------------------------------

volatile unsigned short* vga_buffer = (unsigned short*)0xB8000;
int term_row = 0;
int term_col = 0;

static void scroll_screen(void) {
    for (int row = 1; row < 25; row++) {
        for (int col = 0; col < 80; col++) {
            vga_buffer[(row - 1) * 80 + col] = vga_buffer[row * 80 + col];
        }
    }

    for (int col = 0; col < 80; col++) {
        vga_buffer[24 * 80 + col] = ' ' | (0x07 << 8);
    }

    term_row = 24;
    term_col = 0;
}

void print_char(char c, unsigned char color) {
    if (c == '\n') {
        term_col = 0;
        term_row++;

        if (term_row >= 25) {
            scroll_screen();
        }

        return;
    }

    if (c == '\b') {
        if (term_col > 0) {
            term_col--;
        } else if (term_row > 0) {
            term_row--;
            term_col = 79;
        } else {
            return;
        }

        vga_buffer[term_row * 80 + term_col] = (unsigned short)' ' | (0x07 << 8);
        return;
    }

    vga_buffer[term_row * 80 + term_col] = (unsigned short)c | (color << 8);
    term_col++;

    if (term_col >= 80) {
        term_col = 0;
        term_row++;

        if (term_row >= 25) {
            scroll_screen();
        }
    }
}

void print_string(const char* str, unsigned char color) {
    for (int i = 0; str[i] != '\0'; i++) {
        print_char(str[i], color);
    }
}

static void print_uint(unsigned int value, unsigned char color) {
    char digits[10];
    int n = 0;

    if (value == 0) {
        print_char('0', color);
        return;
    }

    while (value > 0 && n < 10) {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n > 0) {
        print_char(digits[--n], color);
    }
}

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
    print_uint(error_code, 0x4F);

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

const char scancode_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

char cmd_buffer[256];
volatile int cmd_idx = 0;
volatile int cmd_ready = 0;

void keyboard_handler_c(void) {
    unsigned char scancode = inb(0x60);

    if (!(scancode & 0x80)) {
        char c = 0;

        if (scancode < sizeof(scancode_ascii)) {
            c = scancode_ascii[scancode];
        }

        if (c == '\n') {
            cmd_buffer[cmd_idx] = '\0';
            cmd_ready = 1;
            print_char('\n', 0x07);
        } else if (c == '\b') {
            if (cmd_idx > 0) {
                cmd_idx--;
                print_char('\b', 0x07);
            }
        } else if (c) {
            if (cmd_idx < 255) {
                cmd_buffer[cmd_idx++] = c;
                print_char(c, 0x0F);
            }
        }
    }

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
    for (int i = 0; i < 80 * 25; i++) {
        vga_buffer[i] = ' ' | (0x07 << 8);
    }

    term_row = 0;
    term_col = 0;
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
    multiboot_magic = magic;
    multiboot_info_addr = info_addr;

    __asm__ __volatile__("cli");

    clear_screen();

    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (unsigned int)&idt;

    for (int i = 0; i < 32; i++) {
        idt_set_gate((unsigned char)i, (unsigned int)exception_stubs[i], 0x08, 0x8E);
    }

    idt_set_gate(32, (unsigned int)timer_handler_asm, 0x08, 0x8E);
    idt_set_gate(33, (unsigned int)keyboard_handler_asm, 0x08, 0x8E);

    load_idt((unsigned int)&idtp);
    init_pic();
    init_pit(100);

    print_string("=== NanoOS Phase 7: Memory map ===\n", 0x0A);
    print_string("Type 'help' for commands.\n", 0x0E);
    print_string("> ", 0x0B);

    __asm__ __volatile__("sti");

    while (1) {
        if (cmd_ready) {
            // Keep keyboard IRQs from modifying the command while we consume it.
            __asm__ __volatile__("cli");

            if (strcmp(cmd_buffer, "help") == 0) {
                print_string("Commands: help, clear, uptime, ticks, meminfo\n", 0x0E);
            } else if (strcmp(cmd_buffer, "uptime") == 0) {
                print_uptime();
            } else if (strcmp(cmd_buffer, "ticks") == 0) {
                print_string("Timer ticks: ", 0x0E);
                print_uint(timer_ticks, 0x0F);
                print_char('\n', 0x07);
            } else if (strcmp(cmd_buffer, "meminfo") == 0) {
                show_meminfo();
            } else if (strcmp(cmd_buffer, "clear") == 0) {
                clear_screen();
            } else if (strcmp(cmd_buffer, "sleep") == 0) {
                print_string("sleep is not implemented yet.\n", 0x09);
            } else if (cmd_idx > 0) {
                print_string("Unknown command: ", 0x0C);
                print_string(cmd_buffer, 0x0C);
                print_char('\n', 0x07);
            }

            cmd_idx = 0;
            cmd_ready = 0;

            print_string("> ", 0x0B);
            __asm__ __volatile__("sti");
        }

        __asm__ __volatile__("hlt");
    }
}
