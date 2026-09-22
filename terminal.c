#include "terminal.h"
#include "terminal_font.h"

static volatile unsigned short* vga_buffer =
    (unsigned short*)0xB8000;

static unsigned int term_row = 0;
static unsigned int term_col = 0;
static unsigned int prompt_row = 0;
static unsigned int prompt_col = 0;

static char command_buffer[TERMINAL_INPUT_MAX];
static char prompt_text[TERMINAL_PROMPT_MAX];
static unsigned int command_length = 0;

#define TERMINAL_STDIN_BUFFER_MAX 512U
static unsigned char stdin_buffer[TERMINAL_STDIN_BUFFER_MAX];
static unsigned int stdin_head = 0;
static unsigned int stdin_tail = 0;
static unsigned int stdin_count = 0;
static int stdin_active = 0;
static unsigned int cursor_index = 0;
static int command_ready = 0;
static terminal_tab_handler_t tab_handler = 0;

static char history[TERMINAL_HISTORY_MAX][TERMINAL_INPUT_MAX];
static unsigned int history_count = 0;
static int history_position = -1;
static char history_scratch[TERMINAL_INPUT_MAX];
static int history_scratch_valid = 0;

static int extended_scancode = 0;
static int shift_down = 0;
static int ctrl_down = 0;
static int alt_down = 0;
static int caps_lock = 0;
static int language_layout = 0;
static int layout_switch_latch = 0;

/* Track physical key state so hardware typematic repeats do not
 * become duplicate input bytes. Explicit key repeats can be added
 * later as a terminal feature with controlled timing. */
static unsigned char key_down[128];

static unsigned char vga_font_buffer[256U * 32U];

struct terminal_cell {
    char character;
    unsigned char color;
};

static struct terminal_cell scrollback[
    TERMINAL_SCROLLBACK_MAX
][TERMINAL_WIDTH];

static unsigned int scrollback_count = 0;
static unsigned int scrollback_start = 0;
static unsigned int view_offset = 0;
static unsigned short view_snapshot[
    TERMINAL_HEIGHT * TERMINAL_WIDTH
];
static unsigned char cursor_start_value = 0x0EU;

static inline void outb(
    unsigned short port,
    unsigned char data
);

static inline unsigned char inb(
    unsigned short port
);

static void terminal_update_cursor(void);

static unsigned int terminal_irq_save(void) {
    unsigned int flags;
    asm volatile(
        "pushfl\n"
        "popl %0\n"
        "cli\n"
        : "=r"(flags)
    );
    return flags;
}

static void terminal_irq_restore(unsigned int flags) {
    asm volatile(
        "pushl %0\n"
        "popfl\n"
        :
        : "r"(flags)
        : "memory"
    );
}

static void terminal_stdin_reset(void) {
    unsigned int flags = terminal_irq_save();

    stdin_head = 0;
    stdin_tail = 0;
    stdin_count = 0;

    terminal_irq_restore(flags);
}

static void terminal_stdin_push(unsigned char value) {
    unsigned int flags;

    if (!stdin_active) {
        return;
    }

    flags = terminal_irq_save();

    if (stdin_count < TERMINAL_STDIN_BUFFER_MAX) {
        stdin_buffer[stdin_tail] = value;
        stdin_tail =
            (stdin_tail + 1U) %
            TERMINAL_STDIN_BUFFER_MAX;
        stdin_count++;
    }

    terminal_irq_restore(flags);
}

static int terminal_stdin_pop(
    unsigned char* destination,
    unsigned int length
) {
    unsigned int flags;
    unsigned int total = 0;

    if (!destination || length == 0U) {
        return 0;
    }

    flags = terminal_irq_save();

    while (total < length &&
           stdin_count > 0U) {
        destination[total++] =
            stdin_buffer[stdin_head];

        stdin_head =
            (stdin_head + 1U) %
            TERMINAL_STDIN_BUFFER_MAX;

        stdin_count--;
    }

    terminal_irq_restore(flags);
    return (int)total;
}

static void terminal_install_cyrillic_font(void) {
    volatile unsigned char* font_memory =
        (volatile unsigned char*)0xA0000;

    /*
     * Enter VGA font-access mode.
     *
     * The sequence below matches the font access/release
     * path used by SeaBIOS/Linux:
     *   - reset the sequencer,
     *   - expose plane 2 at A0000,
     *   - disable odd/even addressing,
     *   - access the 256 glyphs,
     *   - restore normal text-mode mapping.
     */
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);

    outb(0x3C4, 0x02);
    outb(0x3C5, 0x04);

    outb(0x3C4, 0x04);
    outb(0x3C5, 0x07);

    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);

    outb(0x3CE, 0x04);
    outb(0x3CF, 0x02);

    outb(0x3CE, 0x05);
    outb(0x3CF, 0x00);

    outb(0x3CE, 0x06);
    outb(0x3CF, 0x04);

    /*
     * VGA reserves 32 bytes per glyph.
     * We use the first 16 scanlines and clear the
     * remaining 16 bytes of the modified glyphs.
     */
    for (unsigned int i = 0;
         i < sizeof(vga_font_buffer);
         i++) {
        vga_font_buffer[i] =
            font_memory[i];
    }

    for (unsigned int i = 0;
         i < terminal_cyrillic_glyph_count;
         i++) {
        unsigned int offset =
            (unsigned int)terminal_cyrillic_glyphs[i].code * 32U;

        for (unsigned int row = 0;
             row < 16U;
             row++) {
            vga_font_buffer[offset + row] =
                terminal_cyrillic_glyphs[i].bitmap[row];
        }

        for (unsigned int row = 16U;
             row < 32U;
             row++) {
            vga_font_buffer[offset + row] = 0;
        }
    }

    for (unsigned int i = 0;
         i < sizeof(vga_font_buffer);
         i++) {
        font_memory[i] =
            vga_font_buffer[i];
    }

    /*
     * Release font access and restore ordinary VGA text
     * addressing. The 0x03 Memory Mode value here is
     * deliberate: it is the release sequence used by
     * SeaBIOS/Linux after font access.
     */
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);

    outb(0x3C4, 0x02);
    outb(0x3C5, 0x03);

    outb(0x3C4, 0x04);
    outb(0x3C5, 0x03);

    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);

    outb(0x3CE, 0x04);
    outb(0x3CF, 0x00);

    outb(0x3CE, 0x05);
    outb(0x3CF, 0x10);

    outb(0x3CE, 0x06);
    outb(0x3CF, 0x0E);
}

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

static inline unsigned char inb(
    unsigned short port
) {
    unsigned char result;
    __asm__ __volatile__(
        "inb %1, %0"
        : "=a"(result)
        : "Nd"(port)
    );
    return result;
}

static void terminal_set_cursor_visible(int visible) {
    unsigned char value =
        visible
            ? (unsigned char)(cursor_start_value & 0x1FU)
            : (unsigned char)(cursor_start_value | 0x20U);
    outb(0x3D4, 0x0A);
    outb(0x3D5, value);
}

static void scrollback_store_row(unsigned int row) {
    unsigned int index;
    if (row >= TERMINAL_HEIGHT) return;
    if (scrollback_count < TERMINAL_SCROLLBACK_MAX) {
        index = (scrollback_start + scrollback_count) % TERMINAL_SCROLLBACK_MAX;
        scrollback_count++;
    } else {
        index = scrollback_start;
        scrollback_start = (scrollback_start + 1U) % TERMINAL_SCROLLBACK_MAX;
    }
    for (unsigned int col = 0; col < TERMINAL_WIDTH; col++) {
        unsigned short cell = vga_buffer[row * TERMINAL_WIDTH + col];
        scrollback[index][col].character = (char)(cell & 0x00FFU);
        scrollback[index][col].color = (unsigned char)((cell >> 8) & 0x00FFU);
    }
}

static void scrollback_snapshot_current_screen(void) {
    for (unsigned int i = 0;
         i < TERMINAL_HEIGHT * TERMINAL_WIDTH;
         i++) {
        view_snapshot[i] = vga_buffer[i];
    }
}

static void scrollback_restore_snapshot(void) {
    for (unsigned int i = 0;
         i < TERMINAL_HEIGHT * TERMINAL_WIDTH;
         i++) {
        vga_buffer[i] = view_snapshot[i];
    }
}

static void scrollback_render_view(void) {
    unsigned int total_rows =
        scrollback_count + TERMINAL_HEIGHT;
    unsigned int end_row;
    unsigned int start_row;

    if (view_offset > scrollback_count) {
        view_offset = scrollback_count;
    }

    /*
     * view_offset is the distance from the live screen in rows.
     * The last part of the logical stream is the saved live screen.
     */
    end_row =
        total_rows - view_offset;

    if (end_row >= TERMINAL_HEIGHT) {
        start_row =
            end_row - TERMINAL_HEIGHT;
    } else {
        start_row = 0;
    }

    for (unsigned int screen_row = 0;
         screen_row < TERMINAL_HEIGHT;
         screen_row++) {
        unsigned int logical_row =
            start_row + screen_row;

        if (logical_row < scrollback_count) {
            unsigned int index =
                (scrollback_start + logical_row) %
                TERMINAL_SCROLLBACK_MAX;

            for (unsigned int col = 0;
                 col < TERMINAL_WIDTH;
                 col++) {
                vga_buffer[
                    screen_row * TERMINAL_WIDTH + col
                ] =
                    (unsigned short)(
                        (unsigned char)
                            scrollback[index][col].character
                    ) |
                    ((unsigned short)
                        scrollback[index][col].color << 8);
            }

            continue;
        }

        {
            unsigned int snapshot_row =
                logical_row - scrollback_count;

            if (snapshot_row >= TERMINAL_HEIGHT) {
                /*
                 * This can happen only when the oldest requested
                 * view would begin before the available history.
                 * Fill the unused area with blank cells.
                 */
                for (unsigned int col = 0;
                     col < TERMINAL_WIDTH;
                     col++) {
                    vga_buffer[
                        screen_row * TERMINAL_WIDTH + col
                    ] =
                        (unsigned short)' ' |
                        (0x07 << 8);
                }
                continue;
            }

            for (unsigned int col = 0;
                 col < TERMINAL_WIDTH;
                 col++) {
                vga_buffer[
                    screen_row * TERMINAL_WIDTH + col
                ] =
                    view_snapshot[
                        snapshot_row * TERMINAL_WIDTH + col
                    ];
            }
        }
    }
}

static void terminal_follow_bottom(void) {
    if (view_offset == 0U) return;

    view_offset = 0;
    scrollback_restore_snapshot();
    terminal_set_cursor_visible(1);
    terminal_update_cursor();
}

static void terminal_update_cursor(void) {
    unsigned int position;
    if (view_offset != 0U) return;
    position =
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
    scrollback_store_row(0);

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
    scrollback_count = 0;
    scrollback_start = 0;
    view_offset = 0;

    for (unsigned int row = 0;
         row < TERMINAL_HEIGHT;
         row++) {
        clear_line(row);
    }

    term_row = 0;
    term_col = 0;
    terminal_set_cursor_visible(1);
    terminal_update_cursor();
}

void print_char(char c, unsigned char color) {
    terminal_follow_bottom();
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
        (unsigned short)(unsigned char)c |
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

static void input_position_for_index(
    unsigned int index,
    unsigned int* row,
    unsigned int* col
) {
    unsigned int first_width;

    if (!row || !col) {
        return;
    }

    first_width =
        TERMINAL_WIDTH - prompt_col;

    if (index < first_width) {
        *row = prompt_row;
        *col = prompt_col + index;
        return;
    }

    index -= first_width;

    *row =
        prompt_row +
        1U +
        index / TERMINAL_WIDTH;
    *col =
        index % TERMINAL_WIDTH;
}

static void redraw_input(void) {
    unsigned int end_row;
    unsigned int end_col;
    unsigned int clear_length;

    terminal_follow_bottom();

    input_position_for_index(
        command_length,
        &end_row,
        &end_col
    );

    /*
     * Keep the whole editable command and the cursor visible.
     * TERMINAL_INPUT_MAX is deliberately small enough that a
     * single command can never require more than a few scrolls.
     */
    while (end_row >= TERMINAL_HEIGHT) {
        scroll_screen();

        if (prompt_row > 0U) {
            prompt_row--;
        }

        input_position_for_index(
            command_length,
            &end_row,
            &end_col
        );
    }

    clear_length = TERMINAL_INPUT_MAX;

    /*
     * Clear the complete editable area first. This also removes
     * stale characters left on lower wrapped lines after deleting
     * or replacing a previously longer command.
     */
    for (unsigned int i = 0;
         i < clear_length;
         i++) {
        unsigned int row;
        unsigned int col;

        input_position_for_index(
            i,
            &row,
            &col
        );

        if (row >= TERMINAL_HEIGHT) {
            break;
        }

        vga_buffer[
            row * TERMINAL_WIDTH + col
        ] =
            (unsigned short)' ' |
            (0x07 << 8);
    }

    /*
     * Redraw the complete command from the prompt position.
     * The first line starts after the prompt; following lines
     * use the full 80-column terminal width.
     */
    for (unsigned int i = 0;
         i < command_length;
         i++) {
        unsigned int row;
        unsigned int col;

        input_position_for_index(
            i,
            &row,
            &col
        );

        if (row >= TERMINAL_HEIGHT) {
            break;
        }

        vga_buffer[
            row * TERMINAL_WIDTH + col
        ] =
            (unsigned short)(unsigned char)command_buffer[i] |
            (0x0F << 8);
    }

    input_position_for_index(
        cursor_index,
        &term_row,
        &term_col
    );

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
    if (command_length >= TERMINAL_INPUT_MAX - 1U) {
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

void terminal_set_tab_handler(terminal_tab_handler_t handler) {
    tab_handler = handler;
}

void terminal_replace_command(const char* source) {
    if (!source) {
        return;
    }

    copy_text(command_buffer, source);
    command_length = input_text_length();
    cursor_index = command_length;
    reset_history_navigation();
    redraw_input();
}

void terminal_set_prompt(const char* prompt) {    unsigned int i = 0;

    if (!prompt || *prompt == '\0') {
        prompt_text[0] = '>';
        prompt_text[1] = ' ';
        prompt_text[2] = '\0';
        return;
    }

    while (i + 1U < TERMINAL_PROMPT_MAX &&
           prompt[i] != '\0') {
        prompt_text[i] = prompt[i];
        i++;
    }

    prompt_text[i] = '\0';
}

void terminal_prompt(void) {
    print_string(prompt_text, 0x0B);

    prompt_row = term_row;
    prompt_col = term_col;

    command_length = 0;
    cursor_index = 0;
    command_buffer[0] = 0;
    reset_history_navigation();

    terminal_update_cursor();
}

static int is_letter_scancode(
    unsigned char scancode
) {
    return
        (scancode >= 0x10U && scancode <= 0x19U) ||
        (scancode >= 0x1EU && scancode <= 0x26U) ||
        (scancode >= 0x2CU && scancode <= 0x32U);
}

static unsigned char english_character(
    unsigned char scancode
) {
    static const unsigned char lower[58] = {
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

    static const unsigned char shifted[58] = {
        0, 0,
        '!', '@', '#', '$',
        '%', '^', '&', '*',
        '(', ')', '_', '+',
        8, 9,
        'Q', 'W', 'E', 'R',
        'T', 'Y', 'U', 'I',
        'O', 'P', '{', '}',
        10, 0,
        'A', 'S', 'D', 'F',
        'G', 'H', 'J', 'K',
        'L', ':', 34, '~',
        0, '|', 'Z', 'X',
        'C', 'V', 'B', 'N',
        'M', '<', '>', '?',
        0, '*', 0, ' '
    };

    if (scancode >= sizeof(lower)) {
        return 0;
    }

    if (is_letter_scancode(scancode)) {
        int upper =
            shift_down ^ caps_lock;

        return upper
            ? shifted[scancode]
            : lower[scancode];
    }

    return shift_down
        ? shifted[scancode]
        : lower[scancode];
}

static unsigned char russian_lower_character(
    unsigned char scancode
) {
    static const unsigned char lower[58] = {
        0, 0,
        '1', '2', '3', '4',
        '5', '6', '7', '8',
        '9', '0', '-', '=',
        8, 9,
        0xA9, 0xE6, 0xE3, 0xAA,
        0xA5, 0xAD, 0xA3, 0xE8,
        0xE9, 0xA7, 0xE5, 0xEA,
        10, 0,
        0xE4, 0xEB, 0xA2, 0xA0,
        0xAF, 0xE0, 0xAE, 0xAB,
        0xA4, 0xA6, 0xED, 0xF1,
        0, 0x5C, 0xEF, 0xE7,
        0xE1, 0xAC, 0xA8, 0xE2,
        0xEC, 0xA1, 0xEE, '.',
        0, '*', 0, ' '
    };

    if (scancode >= sizeof(lower)) {
        return 0;
    }

    return lower[scancode];
}

static unsigned char russian_character(
    unsigned char scancode
) {
    unsigned char value =
        russian_lower_character(scancode);

    if (!value) {
        return 0;
    }

    if (is_letter_scancode(scancode)) {
        int upper =
            shift_down ^ caps_lock;

        if (upper) {
            if (value == 0xF1U) {
                return 0xF0U;
            }

            if (value >= 0xA0U &&
                value <= 0xAFU) {
                return (unsigned char)(value - 0x20U);
            }

            if (value >= 0xE0U &&
                value <= 0xEFU) {
                return (unsigned char)(value - 0x50U);
            }

            return value;
        }
    }

    return value;
}

static char scancode_to_ascii(
    unsigned char scancode
) {
    return (char)(
        language_layout
            ? russian_character(scancode)
            : english_character(scancode)
    );
}

static void clear_current_input(void) {
    command_length = 0;
    cursor_index = 0;
    command_buffer[0] = 0;
    reset_history_navigation();
    redraw_input();
}

static void cancel_current_command(void) {
    clear_current_input();

    term_row = prompt_row;
    term_col = prompt_col;

    print_string("^C", 0x0C);
    print_char(10, 0x07);

    terminal_prompt();
}

static void try_switch_layout(void) {
    /*
     * Switch only when both modifiers are physically held.
     * The latch prevents auto-repeat / modifier ordering
     * from toggling the layout several times in one chord.
     */
    if (alt_down &&
        shift_down &&
        !layout_switch_latch) {
        language_layout =
            !language_layout;
        layout_switch_latch = 1;
    }

    if (!alt_down && !shift_down) {
        layout_switch_latch = 0;
    }
}

void terminal_keyboard_scancode(
    unsigned char scancode
) {
    int released =
        (scancode & 0x80U) != 0;

    if (scancode == 0xE0U) {
        extended_scancode = 1;
        return;
    }

    if (extended_scancode) {
        unsigned char code =
            (unsigned char)(scancode & 0x7FU);

        extended_scancode = 0;

        /*
         * Right Ctrl / Right Alt arrive with an E0 prefix.
         * Handle their make and break codes here instead of
         * silently dropping the release event.
         */
        if (code == 0x1DU) {
            ctrl_down = !released;
            return;
        }

        if (code == 0x38U) {
            alt_down = !released;
            try_switch_layout();
            return;
        }

        if (stdin_active) {
            return;
        }

        if (released) {
            return;
        }

        if (command_ready) {
            return;
        }

        if (code == 0x4BU) {
            if (cursor_index > 0U) {
                cursor_index--;
                redraw_input();
            }
            return;
        }

        if (code == 0x4DU) {
            if (cursor_index < command_length) {
                cursor_index++;
                redraw_input();
            }
            return;
        }

        if (code == 0x47U) {
            cursor_index = 0;
            redraw_input();
            return;
        }

        if (code == 0x4FU) {
            cursor_index = command_length;
            redraw_input();
            return;
        }

        if (code == 0x53U) {
            delete_character();
            return;
        }

        if (code == 0x49U) {
            if (scrollback_count > 0U) {
                unsigned int page = TERMINAL_HEIGHT;

                if (view_offset == 0U) {
                    scrollback_snapshot_current_screen();
                }

                if (view_offset + page > scrollback_count) {
                    view_offset = scrollback_count;
                } else {
                    view_offset += page;
                }

                scrollback_render_view();
                terminal_set_cursor_visible(0);
            }
            return;
        }

        if (code == 0x51U) {
            if (view_offset > 0U) {
                unsigned int page = TERMINAL_HEIGHT;

                if (view_offset <= page) {
                    terminal_follow_bottom();
                } else {
                    view_offset -= page;
                    scrollback_render_view();
                    terminal_set_cursor_visible(0);
                }
            }
            return;
        }

        if (view_offset != 0U) {
            terminal_follow_bottom();
        }

        if (code == 0x48U) {
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

        if (code == 0x50U) {
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

    /*
     * Modifier keys: use the base scan code so make and
     * break events are handled symmetrically.
     */
    {
        unsigned char code =
            (unsigned char)(scancode & 0x7FU);

        if (code == 0x2AU ||
            code == 0x36U) {
            shift_down = !released;
            try_switch_layout();
            return;
        }

        if (code == 0x1DU) {
            ctrl_down = !released;
            return;
        }

        if (code == 0x38U) {
            alt_down = !released;
            try_switch_layout();
            return;
        }
    }

    {
        unsigned char code =
            (unsigned char)(scancode & 0x7FU);

        if (released) {
            if (code < sizeof(key_down)) {
                key_down[code] = 0;
            }
            return;
        }

        if (code < sizeof(key_down)) {
            if (key_down[code]) {
                return;
            }

            key_down[code] = 1;
        }
    }

    if (scancode == 0x3AU) {
        caps_lock = !caps_lock;
        return;
    }

    if (stdin_active) {
        if (ctrl_down &&
            scancode == 0x2EU) {
            terminal_stdin_push(3U);
            return;
        }

        if (scancode == 0x1CU) {
            terminal_stdin_push(10U);
            print_char(10, 0x07);
            return;
        }

        if (scancode == 0x0EU) {
            terminal_stdin_push(8U);
            print_char(8, 0x07);
            return;
        }

        if (scancode == 0x0FU) {
            terminal_stdin_push(9U);
            print_string("    ", 0x07);
            return;
        }

        {
            char character =
                scancode_to_ascii(scancode);
            unsigned char value =
                (unsigned char)character;

            if (value >= 0x20U &&
                value <= 0x7EU) {
                terminal_stdin_push(value);
                print_char(character, 0x07);
            } else if (
                language_layout &&
                (
                    (value >= 0x80U && value <= 0xAFU) ||
                    (value >= 0xE0U && value <= 0xEFU) ||
                    value == 0xF0U ||
                    value == 0xF1U
                )
            ) {
                terminal_stdin_push(value);
                print_char(character, 0x07);
            }
        }

        return;
    }

    if (command_ready) {
        return;
    }

    if (view_offset != 0U) {
        terminal_follow_bottom();
    }

    if (ctrl_down) {
        if (scancode == 0x2EU) {
            cancel_current_command();
            return;
        }

        if (scancode == 0x26U) {
            terminal_clear();
            terminal_prompt();
            return;
        }

        if (scancode == 0x16U) {
            clear_current_input();
            return;
        }

        if (scancode == 0x1EU) {
            cursor_index = 0;
            redraw_input();
            return;
        }

        if (scancode == 0x12U) {
            cursor_index = command_length;
            redraw_input();
            return;
        }
    }

    if (scancode == 0x1CU) {
        unsigned int row;
        unsigned int col;

        command_buffer[command_length] = 0;
        command_ready = 1;

        /*
         * Enter always moves to the end of the visible command.
         * If the last typed character already wrapped at column 80,
         * the cursor is already on the next line and another newline
         * would create an unwanted blank line.
         */
        input_position_for_index(
            command_length,
            &row,
            &col
        );

        term_row = row;
        term_col = col;

        if (term_col != 0U) {
            print_char(10, 0x07);
        } else {
            terminal_update_cursor();
        }

        return;
    }

    if (scancode == 0x0EU) {
        backspace_character();
        return;
    }

    if (scancode == 0x0FU) {
        if (tab_handler &&
            cursor_index == command_length) {
            tab_handler();
        } else {
            insert_character(' ');
            insert_character(' ');
            insert_character(' ');
            insert_character(' ');
        }
        return;
    }

    {
        char character =
            scancode_to_ascii(scancode);
        unsigned char value =
            (unsigned char)character;

        if (value == 9U) {
            insert_character(' ');
            insert_character(' ');
            insert_character(' ');
            insert_character(' ');
        } else if (
            value >= 0x20U &&
            value <= 0x7EU
        ) {
            insert_character(character);
        } else if (
            language_layout &&
            (
                (value >= 0x80U && value <= 0xAFU) ||
                (value >= 0xE0U && value <= 0xEFU) ||
                value == 0xF0U ||
                value == 0xF1U
            )
        ) {
            insert_character(character);
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
    shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    caps_lock = 0;
    language_layout = 0;
    layout_switch_latch = 0;
    for (unsigned int i = 0; i < sizeof(key_down); i++) {
        key_down[i] = 0;
    }
    command_buffer[0] = 0;
    tab_handler = 0;
    scrollback_count = 0;
    scrollback_start = 0;
    view_offset = 0;
    stdin_active = 0;
    stdin_head = 0;
    stdin_tail = 0;
    stdin_count = 0;
    for (unsigned int i = 0;
         i < TERMINAL_HEIGHT * TERMINAL_WIDTH;
         i++) {
        view_snapshot[i] = 0;
    }

    outb(0x3D4, 0x0A);
    cursor_start_value = inb(0x3D5);
    terminal_set_cursor_visible(1);

    prompt_text[0] = '>';
    prompt_text[1] = ' ';
    prompt_text[2] = 0;

    terminal_install_cyrillic_font();
    terminal_clear();
}

void terminal_set_layout(int layout) {
    if (layout == 0 || layout == 1) {
        language_layout = layout;
    }
}

int terminal_get_layout(void) {
    return language_layout;
}

const char* terminal_layout_name(void) {
    return language_layout
        ? "RU"
        : "EN";
}


void terminal_set_stdin_active(int active) {
    terminal_stdin_reset();

    /* A new input session starts with no keys logically held. */
    for (unsigned int i = 0; i < sizeof(key_down); i++) {
        key_down[i] = 0;
    }

    stdin_active = active ? 1 : 0;
}

int terminal_read_stdin(
    void* buffer,
    unsigned int length
) {
    if (!stdin_active ||
        !buffer ||
        length == 0U) {
        return 0;
    }

    return terminal_stdin_pop(
        (unsigned char*)buffer,
        length
    );
}
