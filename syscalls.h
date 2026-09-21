#ifndef NANOOS_SYSCALLS_H
#define NANOOS_SYSCALLS_H

#define SYS_EXIT  0U
#define SYS_WRITE 1U
#define SYS_GETPID 2U
#define SYS_YIELD  3U

void syscall_set_kernel_stack(unsigned int stack_top);

int syscall_init(void);
int syscall_dispatch(void* registers);
void syscall_run_test(void);

#endif
