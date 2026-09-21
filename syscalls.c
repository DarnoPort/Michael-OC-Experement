#include "syscalls.h"
#include "memory.h"
#include "paging.h"

extern void print_char(char c, unsigned char color);
extern void print_string(const char* str, unsigned char color);

extern void set_tss_descriptor(unsigned int base, unsigned int limit);
extern void load_tss(void);
extern void enter_user_mode(unsigned int eip, unsigned int esp);

extern char stack_top;
extern char user_program_start;
extern char user_program_end;

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
static unsigned int user_code_physical = VM_ALLOC_FAIL;
static unsigned int user_stack_physical = VM_ALLOC_FAIL;

static void copy_user_program(unsigned int physical) {
    volatile unsigned char* destination =
        (volatile unsigned char*)physical;
    const unsigned char* source =
        (const unsigned char*)&user_program_start;
    const unsigned char* end =
        (const unsigned char*)&user_program_end;

    while (source < end) {
        *destination++ = *source++;
    }
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

    user_code_physical = phys_alloc_page();
    user_stack_physical = phys_alloc_page();

    if (user_code_physical == VM_ALLOC_FAIL ||
        user_stack_physical == VM_ALLOC_FAIL) {
        if (user_code_physical != VM_ALLOC_FAIL) {
            phys_free_page(user_code_physical);
        }

        if (user_stack_physical != VM_ALLOC_FAIL) {
            phys_free_page(user_stack_physical);
        }

        user_code_physical = VM_ALLOC_FAIL;
        user_stack_physical = VM_ALLOC_FAIL;
        return 0;
    }

    copy_user_program(user_code_physical);

    if (!paging_map_user_page(
            USER_CODE_BASE,
            user_code_physical,
            PAGE_PRESENT
        )) {
        phys_free_page(user_code_physical);
        phys_free_page(user_stack_physical);
        user_code_physical = VM_ALLOC_FAIL;
        user_stack_physical = VM_ALLOC_FAIL;
        return 0;
    }

    if (!paging_map_user_page(
            USER_STACK_BASE,
            user_stack_physical,
            PAGE_PRESENT | PAGE_WRITABLE
        )) {
        paging_unmap_user_page(USER_CODE_BASE);
        phys_free_page(user_code_physical);
        phys_free_page(user_stack_physical);
        user_code_physical = VM_ALLOC_FAIL;
        user_stack_physical = VM_ALLOC_FAIL;
        return 0;
    }

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

    if (registers->eax == SYS_EXIT) {
        print_string("\n[syscall] user program exited.\n", 0x0E);
        registers->eax = 0;
        return 1;
    }

    registers->eax = 0xFFFFFFFFU;
    return 0;
}

void syscall_run_test(void) {
    if (user_code_physical == VM_ALLOC_FAIL ||
        user_stack_physical == VM_ALLOC_FAIL) {
        print_string(
            "usertest: user environment is not initialized.\n",
            0x0C
        );
        return;
    }

    print_string("Entering Ring 3...\n", 0x0E);

    enter_user_mode(USER_CODE_BASE, USER_STACK_TOP);

    print_string("Returned to kernel from Ring 3.\n", 0x0E);
}
