#include "terminal.h"

static volatile unsigned short* vga_buffer =
    (unsigned short*)0xB8000;

static unsigned int term_row = 0;
static unsigned int term_col = 0;
static unsigned int prompt_row = 0;
static unsigned int prompt_col = 0;

static char command_buffer[TERMINAL_INPUT_MAX];
static unsigned int command_length = 0;
static unsigned int cursor_index = 0;
static int command_ready = 0;

static char history[TERMINAL_HISTORY_MAX][TERMINAL_INPUT_MAX];
static unsigned int history_count = 0;
static int history_position = -1;
static char history_scratch[TERMINAL_INPUT_MAX];
static int history_scratch_valid = 0;

static int extended_scancode = 0;

static inline void outb(
    unsigned short port,
    unsigned char data
) {
    __asm__ __volatile__(
        "outb %0, %1"
        :
        : "a"(data), "Nd"(port)
    );
}

static void terminal_update_cursor(void) {
    unsigned int position =
        term_row * TERMINAL_WIDTH + term_col;

    outb(0x3D4, 0x0F);
    outb(
        0x3D5,
        (unsigned char)(position & 0xFFU)
    );

    outb(0x3D4, 0x0E);
    outb(
        0x3D5,
        (unsigned char)((position >> 8) & 0xFFU)
    );
}

static void clear_line(unsigned int row) {
    for (unsigned int col = 0;
         col < TERMINAL_WIDTH;
         col++) {
        vga_buffer[
            row * TERMINAL_WIDTH + col
        ] =
            (unsigned short)' ' |
            (0x07 << 8);
    }
}

static void scroll_screen(void) {
    for (unsigned int row = 1;
         row < TERMINAL_HEIGHT;
         row++) {
        for (unsigned int col = 0;
             col < TERMINAL_WIDTH;
             col++) {
            vga_buffer[
                (row - 1U) * TERMINAL_WIDTH + col
            ] =
                vga_buffer[
                    row * TERMINAL_WIDTH + col
                ];
        }
    }

    clear_line(TERMINAL_HEIGHT - 1U);
    term_row = TERMINAL_HEIGHT - 1U;
    term_col = 0;
}

void terminal_clear(void) {
    for (unsigned int row = 0;
         row < TERMINAL_HEIGHT;
         row++) {
        clear_line(row);
    }

    term_row = 0;
    term_col = 0;
    terminal_update_cursor();
}

void print_char(char c, unsigned char color) {
    if ((unsigned char)c == 10U) {
        term_col = 0;
        term_row++;

        if (term_row >= TERMINAL_HEIGHT) {
            scroll_screen();
        }

        terminal_update_cursor();
        return;
    }

    if ((unsigned char)c == 8U) {
        if (term_col > 0U) {
            term_col--;
        } else if (term_row > 0U) {
            term_row--;
            term_col = TERMINAL_WIDTH - 1U;
        } else {
            return;
        }

        vga_buffer[
            term_row * TERMINAL_WIDTH + term_col
        ] =
            (unsigned short)' ' |
            (0x07 << 8);

        terminal_update_cursor();
        return;
    }

    vga_buffer[
        term_row * TERMINAL_WIDTH + term_col
    ] =
        (unsigned short)c |
        (color << 8);

    term_col++;

    if (term_col >= TERMINAL_WIDTH) {
        term_col = 0;
        term_row++;

        if (term_row >= TERMINAL_HEIGHT) {
            scroll_screen();
        }
    }

    terminal_update_cursor();
}

void print_string(
    const char* str,
    unsigned char color
) {
    if (!str) {
        return;
    }

    for (unsigned int i = 0;
         str[i] != 0;
         i++) {
        print_char(str[i], color);
    }
}


static void print_hex_digit(unsigned int value, unsigned char color) {
    const char* hex = "0123456789ABCDEF";
    value &= 0xFU;
    print_char(hex[value], color);
}

void print_hex32(unsigned int value, unsigned char color) {
    print_string("0x", color);

    for (int shift = 28; shift >= 0; shift -= 4) {
        print_hex_digit(value >> shift, color);
    }
}

void print_hex64(unsigned int high, unsigned int low, unsigned char color) {
    print_hex32(high, color);
    print_char('_', color);

    for (int shift = 28; shift >= 0; shift -= 4) {
        print_hex_digit(low >> shift, color);
    }
}

void print_uint(unsigned int value, unsigned char color) {
    char digits[10];
    int n = 0;

    if (value == 0U) {
        print_char('0', color);
        return;
    }

    while (value > 0U && n < 10) {
        digits[n++] = (char)('0' + value % 10U);
        value /= 10U;
    }

    while (n > 0) {
        print_char(digits[--n], color);
    }
}

static void copy_text(
    char* destination,
    const char* source
) {
    unsigned int i = 0;

    if (!destination || !source) {
        return;
    }

    while (i + 1U < TERMINAL_INPUT_MAX &&
           source[i] != 0) {
        destination[i] = source[i];
        i++;
    }

    destination[i] = 0;
}

static unsigned int input_text_length(void) {
    unsigned int length = 0;

    while (length + 1U < TERMINAL_INPUT_MAX &&
           command_buffer[length] != 0) {
        length++;
    }

    return length;
}

static void redraw_input(void) {
    unsigned int max_length =
        TERMINAL_WIDTH - prompt_col;

    if (max_length > 0U) {
        max_length--;
    }

    for (unsigned int i = 0;
         i < max_length;
         i++) {
        char value = ' ';

        if (i < command_length) {
            value = command_buffer[i];
        }

        vga_buffer[
            prompt_row * TERMINAL_WIDTH +
            prompt_col + i
        ] =
            (unsigned short)value |
            ((i < command_length ? 0x0F : 0x07) << 8);
    }

    term_row = prompt_row;
    term_col = prompt_col + cursor_index;

    if (term_col >= TERMINAL_WIDTH) {
        term_col = TERMINAL_WIDTH - 1U;
    }

    terminal_update_cursor();
}

static void reset_history_navigation(void) {
    history_position = -1;
    history_scratch_valid = 0;
}

static void history_store_current(void) {
    if (command_length == 0U) {
        return;
    }

    if (history_count < TERMINAL_HISTORY_MAX) {
        copy_text(
            history[history_count],
            command_buffer
        );
        history_count++;
        return;
    }

    for (unsigned int i = 1U;
         i < TERMINAL_HISTORY_MAX;
         i++) {
        copy_text(
            history[i - 1U],
            history[i]
        );
    }

    copy_text(
        history[TERMINAL_HISTORY_MAX - 1U],
        command_buffer
    );
}

static void load_command(
    const char* source
) {
    copy_text(
        command_buffer,
        source
    );

    command_length =
        input_text_length();
    cursor_index = command_length;
    redraw_input();
}

static void load_history(int position) {
    if (history_count == 0U) {
        return;
    }

    if (!history_scratch_valid) {
        copy_text(
            history_scratch,
            command_buffer
        );
        history_scratch_valid = 1;
    }

    if (position < 0) {
        position = 0;
    }

    if (position >= (int)history_count) {
        history_position = (int)history_count;
        load_command(history_scratch);
        return;
    }

    history_position = position;
    load_command(
        history[history_position]
    );
}

static void insert_character(char c) {
    unsigned int available =
        TERMINAL_WIDTH - prompt_col;

    if (available > 0U) {
        available--;
    }

    if (cursor_index >= available ||
        command_length >= TERMINAL_INPUT_MAX - 1U) {
        return;
    }

    for (unsigned int i = command_length;
         i > cursor_index;
         i--) {
        command_buffer[i] =
            command_buffer[i - 1U];
    }

    command_buffer[cursor_index] = c;
    command_length++;
    cursor_index++;

    reset_history_navigation();
    redraw_input();
}

static void delete_character(void) {
    if (cursor_index >= command_length) {
        return;
    }

    for (unsigned int i = cursor_index;
         i + 1U < command_length;
         i++) {
        command_buffer[i] =
            command_buffer[i + 1U];
    }

    command_length--;
    command_buffer[command_length] = 0;

    reset_history_navigation();
    redraw_input();
}

static void backspace_character(void) {
    if (cursor_index == 0U) {
        return;
    }

    for (unsigned int i = cursor_index - 1U;
         i + 1U < command_length;
         i++) {
        command_buffer[i] =
            command_buffer[i + 1U];
    }

    command_length--;
    cursor_index--;
    command_buffer[command_length] = 0;

    reset_history_navigation();
    redraw_input();
}

void terminal_prompt(void) {
    print_string("> ", 0x0B);

    prompt_row = term_row;
    prompt_col = term_col;

    command_length = 0;
    cursor_index = 0;
    command_buffer[0] = 0;
    reset_history_navigation();

    terminal_update_cursor();
}

static char scancode_to_ascii(
    unsigned char scancode
) {
    static const unsigned char codes[58] = {
        0, 0,
        '1', '2', '3', '4',
        '5', '6', '7', '8',
        '9', '0', '-', '=',
        8, 9,
        'q', 'w', 'e', 'r',
        't', 'y', 'u', 'i',
        'o', 'p', '[', ']',
        10, 0,
        'a', 's', 'd', 'f',
        'g', 'h', 'j', 'k',
        'l', ';', 39, 96,
        0, 92, 'z', 'x',
        'c', 'v', 'b', 'n',
        'm', ',', '.', '/',
        0, '*', 0, ' '
    };

    if (scancode >= sizeof(codes)) {
        return 0;
    }

    return (char)codes[scancode];
}

void terminal_keyboard_scancode(
    unsigned char scancode
) {
    if (scancode == 0xE0U) {
        extended_scancode = 1;
        return;
    }

    if (scancode & 0x80U) {
        extended_scancode = 0;
        return;
    }

    if (extended_scancode) {
        extended_scancode = 0;

        if (command_ready) {
            return;
        }

        if (scancode == 0x4BU) {
            if (cursor_index > 0U) {
                cursor_index--;
                redraw_input();
            }
            return;
        }

        if (scancode == 0x4DU) {
            if (cursor_index < command_length) {
                cursor_index++;
                redraw_input();
            }
            return;
        }

        if (scancode == 0x47U) {
            cursor_index = 0;
            redraw_input();
            return;
        }

        if (scancode == 0x4FU) {
            cursor_index = command_length;
            redraw_input();
            return;
        }

        if (scancode == 0x53U) {
            delete_character();
            return;
        }

        if (scancode == 0x48U) {
            if (history_count == 0U) {
                return;
            }

            if (history_position < 0) {
                history_position =
                    (int)history_count;
            }

            if (history_position > 0) {
                load_history(
                    history_position - 1
                );
            }

            return;
        }

        if (scancode == 0x50U) {
            if (history_position < 0) {
                return;
            }

            if (history_position <
                (int)history_count) {
                load_history(
                    history_position + 1
                );
            } else {
                load_history(
                    (int)history_count
                );
            }

            return;
        }

        return;
    }

    if (command_ready) {
        return;
    }

    if (scancode == 0x1CU) {
        command_buffer[command_length] = 0;
        command_ready = 1;
        print_char(10, 0x07);
        return;
    }

    if (scancode == 0x0EU) {
        backspace_character();
        return;
    }

    {
        char c =
            scancode_to_ascii(scancode);

        if ((unsigned char)c == 9U) {
            insert_character(' ');
            insert_character(' ');
            insert_character(' ');
            insert_character(' ');
        } else if (
            (unsigned char)c >= 0x20U &&
            (unsigned char)c <= 0x7EU
        ) {
            insert_character(c);
        }
    }
}

int terminal_command_ready(void) {
    return command_ready;
}

const char* terminal_get_command(void) {
    return command_buffer;
}

unsigned int terminal_command_length(void) {
    return command_length;
}

void terminal_command_consumed(void) {
    history_store_current();

    command_ready = 0;
    command_length = 0;
    cursor_index = 0;
    command_buffer[0] = 0;
    reset_history_navigation();

    terminal_prompt();
}

void terminal_print_history(void) {
    if (history_count == 0U) {
        print_string(
            "No command history.",
            0x08
        );
        print_char(10, 0x07);
        return;
    }

    for (unsigned int i = 0;
         i < history_count;
         i++) {
        unsigned int number = i + 1U;
        char digits[10];
        int count = 0;

        while (number > 0U && count < 10) {
            digits[count++] =
                (char)(
                    '0' + number % 10U
                );
            number /= 10U;
        }

        print_string("  ", 0x07);

        while (count > 0) {
            print_char(
                digits[--count],
                0x0F
            );
        }

        print_string("  ", 0x07);
        print_string(
            history[i],
            0x0F
        );
        print_char(10, 0x07);
    }
}

void terminal_init(void) {
    command_length = 0;
    cursor_index = 0;
    command_ready = 0;
    history_count = 0;
    history_position = -1;
    history_scratch_valid = 0;
    extended_scancode = 0;
    command_buffer[0] = 0;

    terminal_clear();
}
