#ifndef NANOOS_PROCESS_H
#define NANOOS_PROCESS_H

#define PROCESS_MAX 8
#define PROCESS_NAME_MAX 16

#define PROCESS_UNUSED    0
#define PROCESS_RUNNABLE  1
#define PROCESS_TERMINATED 2

struct process {
    unsigned int pid;
    unsigned int state;

    unsigned int user_code_physical;
    unsigned int user_stack_physical;
    unsigned int kernel_stack_physical;
    unsigned int kernel_stack_top;

    unsigned int saved_esp;
    int started;

    char name[PROCESS_NAME_MAX];
};

void scheduler_init(void);
int process_create(const char* name);
int scheduler_prepare_first(void);
unsigned int scheduler_on_timer(unsigned int* interrupt_stack);
unsigned int scheduler_on_syscall(unsigned int* interrupt_stack);

int scheduler_current_pid(void);
int process_exit_current(void);

void scheduler_print_processes(void);
void scheduler_cleanup(void);

#endif
