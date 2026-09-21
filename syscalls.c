#include "syscalls.h"
#include "memory.h"
#include "paging.h"
#include "process.h"

extern void print_char(char c, unsigned char color);
extern void print_string(const char* str, unsigned char color);

extern void set_tss_descriptor(unsigned int base, unsigned int limit);
extern void load_tss(void);
extern void enter_user_mode(unsigned int eip, unsigned int esp);

extern char stack_top;
volatile unsigned int user_return_esp = 0;

struct tss32 {
    unsigned int prev_tss;
    unsigned int esp0;
    unsigned int ss0;
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax;
    unsigned int ecx;
    unsigned int edx;
    unsigned int ebx;
    unsigned int esp;
    unsigned int ebp;
    unsigned int esi;
    unsigned int edi;
    unsigned int es;
    unsigned int cs;
    unsigned int ss;
    unsigned int ds;
    unsigned int fs;
    unsigned int gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap_base;
} __attribute__((packed));

struct syscall_registers {
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp_dummy;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
};

static struct tss32 tss;
void syscall_set_kernel_stack(unsigned int stack_top) {
    tss.esp0 = stack_top;
}

int syscall_init(void) {
    for (unsigned int i = 0; i < sizeof(tss); i++) {
        ((unsigned char*)&tss)[i] = 0;
    }

    tss.esp0 = (unsigned int)(unsigned long)&stack_top;
    tss.ss0 = 0x10;
    tss.iomap_base = (unsigned short)sizeof(tss);

    set_tss_descriptor(
        (unsigned int)(unsigned long)&tss,
        sizeof(tss) - 1U
    );
    load_tss();

    scheduler_init();
    return 1;
}

int syscall_dispatch(void* registers_ptr) {
    struct syscall_registers* registers =
        (struct syscall_registers*)registers_ptr;

    if (registers->eax == SYS_WRITE) {
        unsigned int address = registers->ebx;
        unsigned int length = registers->ecx;

        if (length == 0 || length > 4096U ||
            !paging_user_range_valid(address, length, 0)) {
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        for (unsigned int i = 0; i < length; i++) {
            print_char(((const char*)address)[i], 0x0F);
        }

        registers->eax = length;
        return 0;
    }

    if (registers->eax == SYS_GETPID) {
        int pid = scheduler_current_pid();

        registers->eax =
            (pid < 0) ? 0xFFFFFFFFU : (unsigned int)pid;
        return 0;
    }

    if (registers->eax == SYS_SBRK) {
        unsigned int old_break;

        if (!process_sbrk(
                registers->ebx,
                &old_break
            )) {
            registers->eax = 0xFFFFFFFFU;
        } else {
            registers->eax = old_break;
        }

        return 0;
    }

    if (registers->eax == SYS_YIELD) {
        registers->eax = 0;
        return 2;
    }

    if (registers->eax == SYS_EXIT) {
        print_string("\n[syscall] user process exited.\n", 0x0E);
        process_exit_current();
        registers->eax = 0;
        return 2;
    }

    registers->eax = 0xFFFFFFFFU;
    return 0;
}

void syscall_run_test(void) {
    int pid_a;
    int pid_b;

    pid_a = process_create("worker-A");
    pid_b = process_create("worker-B");

    if (pid_a < 0) {
        print_string(
            "usertest: failed to create first process.\n",
            0x0C
        );
        scheduler_cleanup();
        return;
    }

    if (pid_b < 0) {
        print_string(
            "usertest: second process could not be created; running one process.\n",
            0x0C
        );
    }

    if (!scheduler_prepare_first()) {
        print_string(
            "usertest: scheduler initialization failed.\n",
            0x0C
        );
        scheduler_cleanup();
        return;
    }

    print_string("Process table:\n", 0x0A);
    scheduler_print_processes();

    print_string("Starting preemptive scheduler...\n", 0x0E);
    print_string("Loading embedded ELF image...\n", 0x0E);
    print_string("Entering Ring 3...\n", 0x0E);

    enter_user_mode(
        scheduler_current_entry(),
        scheduler_current_stack_top()
    );

    print_string("All user processes have returned to the kernel.\n", 0x0E);

    scheduler_cleanup();
}
