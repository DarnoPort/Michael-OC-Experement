#ifndef NANOOS_TERMINAL_H
#define NANOOS_TERMINAL_H

#define TERMINAL_WIDTH 80U
#define TERMINAL_HEIGHT 25U
#define TERMINAL_SCROLLBACK_MAX 256U
#define TERMINAL_INPUT_MAX 256U
#define TERMINAL_HISTORY_MAX 16U
#define TERMINAL_PROMPT_MAX 64U

void terminal_init(void);
void terminal_clear(void);

void print_char(char c, unsigned char color);
void print_string(const char* str, unsigned char color);
void print_uint(unsigned int value, unsigned char color);
void print_hex32(unsigned int value, unsigned char color);
void print_hex64(unsigned int high, unsigned int low, unsigned char color);

typedef void (*terminal_tab_handler_t)(void);

void terminal_set_prompt(const char* prompt);
void terminal_set_tab_handler(terminal_tab_handler_t handler);
void terminal_replace_command(const char* command);
void terminal_prompt(void);
void terminal_keyboard_scancode(unsigned char scancode);

int terminal_command_ready(void);
const char* terminal_get_command(void);
unsigned int terminal_command_length(void);
void terminal_command_consumed(void);

void terminal_print_history(void);
void terminal_set_layout(int layout);
int terminal_get_layout(void);
const char* terminal_layout_name(void);

void terminal_set_stdin_active(int active);
int terminal_read_stdin(
    void* buffer,
    unsigned int length
);

#endif
