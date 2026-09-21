#ifndef NANOOS_SYSCALLS_H
#define NANOOS_SYSCALLS_H

#define SYS_EXIT  0U
#define SYS_WRITE 1U

int syscall_init(void);
int syscall_dispatch(void* registers);
void syscall_run_test(void);

#endif
