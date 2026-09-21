#ifndef NANOOS_TERMINAL_H
#define NANOOS_TERMINAL_H

#define TERMINAL_WIDTH 80U
#define TERMINAL_HEIGHT 25U
#define TERMINAL_INPUT_MAX 77U
#define TERMINAL_HISTORY_MAX 16U

void terminal_init(void);
void terminal_clear(void);

void print_char(char c, unsigned char color);
void print_string(const char* str, unsigned char color);

void terminal_prompt(void);
void terminal_keyboard_scancode(unsigned char scancode);

int terminal_command_ready(void);
const char* terminal_get_command(void);
unsigned int terminal_command_length(void);
void terminal_command_consumed(void);

void terminal_print_history(void);

#endif
