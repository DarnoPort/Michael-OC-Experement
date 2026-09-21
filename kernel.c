// --- 1. Работа с портами ---
static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ __volatile__("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(unsigned short port, unsigned char data) {
    __asm__ __volatile__("outb %0, %1" : : "a"(data), "Nd"(port));
}

// --- 2. Видеодрайвер ---
volatile unsigned short* vga_buffer = (unsigned short*)0xB8000;
int term_row = 0, term_col = 0;

void print_char(char c, unsigned char color) {
    if (c == '\n') { term_col = 0; term_row++; return; }
    if (c == '\b') {
        if (term_col > 0) term_col--; else if (term_row > 0) { term_row--; term_col = 79; }
        vga_buffer[term_row * 80 + term_col] = (unsigned short)' ' | (0x07 << 8);
        return;
    }
    vga_buffer[term_row * 80 + term_col] = (unsigned short)c | (color << 8);
    if (++term_col >= 80) { term_col = 0; term_row++; }
}

void print_string(const char* str, unsigned char color) {
    for (int i = 0; str[i] != '\0'; i++) print_char(str[i], color);
}

// --- 3. Структуры IDT ---
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

// Внешние функции из interrupts.asm
extern void load_idt(unsigned int);
extern void keyboard_handler_asm();
extern void timer_handler_asm();
extern void timer_handler_c();

void idt_set_gate(unsigned char num, unsigned int base, unsigned short sel, unsigned char flags) {
    idt[num].base_lo = base & 0xFFFF;
    idt[num].base_hi = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

// --- 4. Настройка контроллера прерываний (PIC) ---
void init_pic() {
    // Инициализация PIC (перенос аппаратных прерываний (IRQ) на векторы 32-47)
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28); // IRQ0-7 теперь начинаются с INT 32
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    
    // Разрешаем ТОЛЬКО прерывание клавиатуры (IRQ1), остальные глушим (Masking)
    // 0xFD = 1111 1101 в двоичном коде (0 значит "включено")
    outb(0x21, 0xFC); 
    outb(0xA1, 0xFF);
}

// --- 5. Драйвер клавиатуры (Interrupt Driven!) ---
const char scancode_ascii[] = {
    0, 0, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
};

char cmd_buffer[256];
int cmd_idx = 0;
volatile int cmd_ready = 0; // Флаг: "команда готова к обработке"



// Эту функцию вызывает ассемблерный обработчик каждый раз при нажатии
void keyboard_handler_c() {
    unsigned char scancode = inb(0x60);
    
    if (!(scancode & 0x80)) { // Если это нажатие (а не отпускание)
        char c = 0;
        if (scancode < sizeof(scancode_ascii)) c = scancode_ascii[scancode];
        
        if (c == '\n') {
            cmd_buffer[cmd_idx] = '\0';
            cmd_ready = 1; // Сигнализируем главному циклу
            print_char('\n', 0x07);
        } else if (c == '\b') {
            if (cmd_idx > 0) { cmd_idx--; print_char('\b', 0x07); }
        } else if (c) {
            if (cmd_idx < 255) { cmd_buffer[cmd_idx++] = c; print_char(c, 0x0F); }
        }
    }
    
    // ВАЖНО: Говорим PIC "я обработал прерывание" (End of Interrupt)
    // Если этого не сделать, клавиатура зависнет навсегда.
    outb(0x20, 0x20);
}


// --- 5. PIT timer ---
volatile unsigned int timer_ticks = 0;

void timer_handler_c() {
    timer_ticks++;
}

void init_pit(unsigned int frequency) {
    unsigned int divisor = 1193182 / frequency;
    if (divisor < 1) divisor = 1;
    if (divisor > 65535) divisor = 65535;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}
// --- 6. Главный цикл ---
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned const char*)s1 - *(unsigned const char*)s2;
}

void clear_screen() {
    for (int i = 0; i < 80 * 25; i++) vga_buffer[i] = ' ' | (0x07 << 8);
    term_row = 0; term_col = 0;
}

extern void dummy_handler_asm();

void kernel_main() {
    clear_screen();
    
    // Настройка IDT
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (unsigned int)&idt;

    // ДОБАВИТЬ ЭТОТ ЦИКЛ: Заполняем ВСЕ 256 векторов заглушками!
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, (unsigned int)dummy_handler_asm, 0x08, 0x8E);
    }
    
    // Регистрируем наш обработчик клавиатуры на вектор 33 (IRQ1 = 32 + 1)
    // 0x08 - это селектор сегмента кода, 0x8E - флаги (Interrupt Gate)
    idt_set_gate(32, (unsigned int)timer_handler_asm, 0x08, 0x8E);
    idt_set_gate(33, (unsigned int)keyboard_handler_asm, 0x08, 0x8E);
    
    load_idt((unsigned int)&idtp);
    init_pic();
    init_pit(100);
    
    __asm__ __volatile__("sti"); // Включаем аппаратные прерывания (Set Interrupts)

    print_string("=== NanoOS Phase 6: Timer + Keyboard ===\n", 0x0A);
    print_string("PIT timer: 100 Hz. CPU sleeps with HLT between interrupts.\n", 0x0E);
    print_string("> ", 0x0B);

    // Теперь у нас "умный" бесконечный цикл
    while (1) {
        if (cmd_ready) {
            if (strcmp(cmd_buffer, "help") == 0) print_string("Commands: help, clear, uptime, ticks, sleep\n", 0x0E);
            else if (strcmp(cmd_buffer, "uptime") == 0) {
                print_string("Uptime ticks: ", 0x0E);
                char digits[11]; int n = 0; unsigned int value = timer_ticks;
                if (value == 0) digits[n++] = '0';
                while (value > 0 && n < 10) { digits[n++] = '0' + (value % 10); value /= 10; }
                while (n > 0) print_char(digits[--n], 0x0F);
                print_string(" (100 ticks/sec)\n", 0x0E);
            }
            else if (strcmp(cmd_buffer, "ticks") == 0) {
                print_string("Timer ticks: ", 0x0E);
                char digits[11]; int n = 0; unsigned int value = timer_ticks;
                if (value == 0) digits[n++] = '0';
                while (value > 0 && n < 10) { digits[n++] = '0' + (value % 10); value /= 10; }
                while (n > 0) print_char(digits[--n], 0x0F);
                print_char('\n', 0x07);
            }
            else if (strcmp(cmd_buffer, "clear") == 0) clear_screen();
            else if (strcmp(cmd_buffer, "sleep") == 0) print_string("Zzz... Just press a key to wake me up.\n", 0x09);
            else if (cmd_idx > 0) {
                print_string("Unknown command: ", 0x0C);
                print_string(cmd_buffer, 0x0C);
                print_char('\n', 0x07);
            }
            
            cmd_idx = 0;
            cmd_ready = 0;
            print_string("> ", 0x0B);
        }
        
        // МАГИЯ ЗДЕСЬ: Инструкция HLT (Halt) полностью останавливает CPU.
        // Он проснется ТОЛЬКО когда ты нажмешь клавишу на клавиатуре!
        __asm__ __volatile__("hlt"); 
    }
}