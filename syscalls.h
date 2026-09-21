#ifndef NANOOS_SYSCALLS_H
#define NANOOS_SYSCALLS_H

#define SYS_EXIT   0U
#define SYS_WRITE  1U
#define SYS_GETPID 2U
#define SYS_YIELD  3U
#define SYS_SBRK        4U
#define SYS_OPEN        5U
#define SYS_FILE_READ   6U
#define SYS_FILE_WRITE  7U
#define SYS_CLOSE       8U
#define SYS_EXEC        9U

void syscall_set_kernel_stack(unsigned int stack_top);

int syscall_init(void);
int syscall_dispatch(void* registers);
void syscall_run_test(void);

#endif
