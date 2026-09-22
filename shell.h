#ifndef NANOOS_SHELL_H
#define NANOOS_SHELL_H

int shell_init(void);
void shell_handle_tab_completion(void);
int shell_handle_command(const char* command);
void shell_close_all(void);

#endif
