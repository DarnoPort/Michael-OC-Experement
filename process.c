#include "process.h"
#include "memory.h"
#include "paging.h"

extern void print_char(char c, unsigned char color);
extern void print_string(const char* str, unsigned char color);
extern void syscall_set_kernel_stack(unsigned int stack_top);

extern char user_program_start;
extern char user_program_end;

#define KERNEL_DATA_SELECTOR 0x10U
#define USER_CODE_SELECTOR   0x1BU
#define USER_DATA_SELECTOR   0x23U
#define USER_CODE_BASE       0x80000000U
#define USER_STACK_BASE      0x80001000U
#define USER_STACK_TOP       0x80002000U

static struct process processes[PROCESS_MAX];
static int current_index = -1;
static unsigned int next_pid = 1;
static int scheduler_active = 0;

static void copy_string(char* destination, const char* source) {
    unsigned int i = 0;

    while (i + 1U < PROCESS_NAME_MAX && source[i] != '\0') {
        destination[i] = source[i];
        i++;
    }

    destination[i] = '\0';
}

static void print_uint(unsigned int value, unsigned char color) {
    char digits[10];
    int count = 0;

    if (value == 0) {
        print_char('0', color);
        return;
    }

    while (value > 0 && count < 10) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }

    while (count > 0) {
        print_char(digits[--count], color);
    }
}

static int process_is_runnable(int index) {
    return index >= 0 &&
           index < PROCESS_MAX &&
           processes[index].state == PROCESS_RUNNABLE;
}

static void activate_process(int index) {
    struct process* process = &processes[index];

    /*
     * Every process receives its own physical code and stack pages.
     * Only the currently running process is mapped into the fixed user
     * virtual addresses. This gives us process-private memory without
     * introducing separate page directories yet.
     */
    paging_map_user_page(
        USER_CODE_BASE,
        process->user_code_physical,
        PAGE_PRESENT
    );

    paging_map_user_page(
        USER_STACK_BASE,
        process->user_stack_physical,
        PAGE_PRESENT | PAGE_WRITABLE
    );

    syscall_set_kernel_stack(process->kernel_stack_top);
}

static int find_first_runnable(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_is_runnable(i)) {
            return i;
        }
    }

    return -1;
}

static int find_next_runnable(int from_index) {
    for (unsigned int offset = 1; offset <= PROCESS_MAX; offset++) {
        int index = (from_index + (int)offset) % PROCESS_MAX;

        if (process_is_runnable(index)) {
            return index;
        }
    }

    return -1;
}

static void build_initial_context(struct process* process) {
    unsigned int* stack =
        (unsigned int*)(unsigned long)process->kernel_stack_top;

    /*
     * The timer/syscall assembly handler uses:
     *
     *   pushad
     *   ...
     *   popad
     *   iretd
     *
     * So we build a fake pushad frame followed by a normal privilege-return
     * frame. The process can therefore start through exactly the same
     * return path used after a context switch.
     */

    *--stack = USER_DATA_SELECTOR;  /* SS */
    *--stack = USER_STACK_TOP;      /* ESP */
    *--stack = 0x202U;              /* EFLAGS: IF=1 */
    *--stack = USER_CODE_SELECTOR;  /* CS */
    *--stack = USER_CODE_BASE;      /* EIP */

    *--stack = 0; /* EAX */
    *--stack = 0; /* ECX */
    *--stack = 0; /* EDX */
    *--stack = 0; /* EBX */
    *--stack = 0; /* original ESP, ignored by popad */
    *--stack = 0; /* EBP */
    *--stack = 0; /* ESI */
    *--stack = 0; /* EDI */

    process->saved_esp = (unsigned int)(unsigned long)stack;
    process->started = 0;
}

void scheduler_init(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        processes[i].pid = 0;
        processes[i].state = PROCESS_UNUSED;
        processes[i].user_code_physical = VM_ALLOC_FAIL;
        processes[i].user_stack_physical = VM_ALLOC_FAIL;
        processes[i].kernel_stack_physical = VM_ALLOC_FAIL;
        processes[i].kernel_stack_top = 0;
        processes[i].saved_esp = 0;
        processes[i].started = 0;
        processes[i].name[0] = '\0';
    }

    current_index = -1;
    next_pid = 1;
    scheduler_active = 0;
}

int process_create(const char* name) {
    int slot = -1;
    unsigned int program_size;
    struct process* process;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROCESS_UNUSED) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        return -1;
    }

    program_size =
        (unsigned int)(
            (unsigned long)&user_program_end -
            (unsigned long)&user_program_start
        );

    if (program_size == 0 || program_size > PAGE_SIZE) {
        return -1;
    }

    process = &processes[slot];

    process->user_code_physical = phys_alloc_page();
    process->user_stack_physical = phys_alloc_page();
    process->kernel_stack_physical = phys_alloc_page();

    if (process->user_code_physical == VM_ALLOC_FAIL ||
        process->user_stack_physical == VM_ALLOC_FAIL ||
        process->kernel_stack_physical == VM_ALLOC_FAIL) {

        if (process->user_code_physical != VM_ALLOC_FAIL) {
            phys_free_page(process->user_code_physical);
        }

        if (process->user_stack_physical != VM_ALLOC_FAIL) {
            phys_free_page(process->user_stack_physical);
        }

        if (process->kernel_stack_physical != VM_ALLOC_FAIL) {
            phys_free_page(process->kernel_stack_physical);
        }

        process->user_code_physical = VM_ALLOC_FAIL;
        process->user_stack_physical = VM_ALLOC_FAIL;
        process->kernel_stack_physical = VM_ALLOC_FAIL;
        return -1;
    }

    {
        volatile unsigned char* destination =
            (volatile unsigned char*)(unsigned long)process->user_code_physical;
        const unsigned char* source =
            (const unsigned char*)&user_program_start;

        for (unsigned int i = 0; i < program_size; i++) {
            destination[i] = source[i];
        }
    }

    for (unsigned int i = 0; i < PAGE_SIZE; i++) {
        ((volatile unsigned char*)(unsigned long)
             process->user_stack_physical)[i] = 0;
    }

    for (unsigned int i = 0; i < PAGE_SIZE; i++) {
        ((volatile unsigned char*)(unsigned long)
             process->kernel_stack_physical)[i] = 0;
    }

    process->kernel_stack_top =
        process->kernel_stack_physical + PAGE_SIZE;

    build_initial_context(process);

    process->pid = next_pid++;
    if (next_pid == 0) {
        next_pid = 1;
    }

    process->state = PROCESS_RUNNABLE;
    process->started = 0;
    copy_string(process->name, name);

    return (int)process->pid;
}

int scheduler_prepare_first(void) {
    int first;

    if (scheduler_active) {
        return 0;
    }

    first = find_first_runnable();
    if (first < 0) {
        return 0;
    }

    current_index = first;
    scheduler_active = 1;

    activate_process(current_index);
    return 1;
}

static unsigned int switch_to_next(
    unsigned int* interrupt_stack,
    int save_current
) {
    int next;

    if (current_index >= 0 &&
        process_is_runnable(current_index) &&
        save_current) {
        processes[current_index].saved_esp =
            (unsigned int)(unsigned long)interrupt_stack;
        processes[current_index].started = 1;
    }

    next = find_next_runnable(current_index);

    if (next < 0) {
        if (current_index >= 0 &&
            process_is_runnable(current_index)) {
            /*
             * A yield with no other runnable process simply resumes the
             * current process from the saved interrupt frame.
             */
            return processes[current_index].saved_esp;
        }

        scheduler_active = 0;
        current_index = -1;
        return 0;
    }

    current_index = next;
    activate_process(current_index);

    return processes[current_index].saved_esp;
}

unsigned int scheduler_on_timer(unsigned int* interrupt_stack) {
    int next;

    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index)) {
        return 0;
    }

    /*
     * The timer can interrupt both Ring 0 and Ring 3. The scheduler only
     * switches user processes when the saved CS has RPL 3.
     *
     * For a kernel-mode interrupt the ordinary handler must return to the
     * shell without touching the process context.
     */
    if ((interrupt_stack[9] & 3U) != 3U) {
        return 0;
    }

    processes[current_index].saved_esp =
        (unsigned int)(unsigned long)interrupt_stack;
    processes[current_index].started = 1;

    next = find_next_runnable(current_index);

    if (next < 0 || next == current_index) {
        return 0;
    }

    current_index = next;
    activate_process(current_index);

    return processes[current_index].saved_esp;
}

unsigned int scheduler_on_syscall(unsigned int* interrupt_stack) {
    if (!scheduler_active ||
        current_index < 0) {
        return 0;
    }

    if ((interrupt_stack[9] & 3U) != 3U) {
        return 0;
    }

    return switch_to_next(
        interrupt_stack,
        processes[current_index].state == PROCESS_RUNNABLE
    );
}

int scheduler_current_pid(void) {
    if (!scheduler_active ||
        current_index < 0 ||
        processes[current_index].state == PROCESS_UNUSED) {
        return -1;
    }

    return (int)processes[current_index].pid;
}

int process_exit_current(void) {
    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index)) {
        return 0;
    }

    processes[current_index].state = PROCESS_TERMINATED;
    return 1;
}

void scheduler_print_processes(void) {
    print_string("PID   STATE       NAME\n", 0x0A);

    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process* process = &processes[i];

        if (process->state == PROCESS_UNUSED) {
            continue;
        }

        print_uint(process->pid, 0x0F);
        print_string("     ", 0x07);

        if (process->state == PROCESS_RUNNABLE) {
            print_string("RUNNABLE    ", 0x0E);
        } else {
            print_string("TERMINATED   ", 0x08);
        }

        print_string(process->name, 0x0F);

        if (i == current_index) {
            print_string("  <current>", 0x0B);
        }

        print_char('\n', 0x07);
    }
}

void scheduler_cleanup(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process* process = &processes[i];

        if (process->state == PROCESS_UNUSED) {
            continue;
        }

        if (process->user_code_physical != VM_ALLOC_FAIL) {
            phys_free_page(process->user_code_physical);
        }

        if (process->user_stack_physical != VM_ALLOC_FAIL) {
            phys_free_page(process->user_stack_physical);
        }

        if (process->kernel_stack_physical != VM_ALLOC_FAIL) {
            phys_free_page(process->kernel_stack_physical);
        }

        process->pid = 0;
        process->state = PROCESS_UNUSED;
        process->user_code_physical = VM_ALLOC_FAIL;
        process->user_stack_physical = VM_ALLOC_FAIL;
        process->kernel_stack_physical = VM_ALLOC_FAIL;
        process->kernel_stack_top = 0;
        process->saved_esp = 0;
        process->started = 0;
        process->name[0] = '\0';
    }

    paging_unmap_user_page(USER_CODE_BASE);
    paging_unmap_user_page(USER_STACK_BASE);

    current_index = -1;
    scheduler_active = 0;
}
