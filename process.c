#include "process.h"
#include "memory.h"
#include "paging.h"
#include "elf.h"
#include "vfs.h"

extern void print_char(char c, unsigned char color);
extern void print_string(const char* str, unsigned char color);
extern void syscall_set_kernel_stack(unsigned int stack_top);
extern char stack_top;
extern void enter_user_mode(unsigned int eip, unsigned int esp);
extern const unsigned char user_image_start;
extern const unsigned char user_image_end;

#define USER_STACK_BASE (USER_STACK_TOP - PAGE_SIZE)

#define USER_CODE_SELECTOR 0x1BU
#define USER_DATA_SELECTOR 0x23U

static struct process processes[PROCESS_MAX];
static int current_index = -1;
static unsigned int next_pid = 1;
static int scheduler_active = 0;

static void copy_string(
    char* destination,
    const char* source
) {
    unsigned int i = 0;

    if (!source) {
        destination[0] = '\0';
        return;
    }

    while (i + 1U < PROCESS_NAME_MAX &&
           source[i] != '\0') {
        destination[i] = source[i];
        i++;
    }

    destination[i] = '\0';
}

static void print_uint(
    unsigned int value,
    unsigned char color
) {
    char digits[10];
    int count = 0;

    if (value == 0) {
        print_char('0', color);
        return;
    }

    while (value > 0 && count < 10) {
        digits[count++] =
            (char)('0' + value % 10U);
        value /= 10U;
    }

    while (count > 0) {
        print_char(
            digits[--count],
            color
        );
    }
}

static int process_is_runnable(int index) {
    return index >= 0 &&
           index < PROCESS_MAX &&
           processes[index].state == PROCESS_RUNNABLE;
}

static void activate_process(int index) {
    struct process* process =
        &processes[index];

    paging_switch_directory(process->cr3);
    /* TSS.esp0 must point one byte past the allocated stack page. */
    syscall_set_kernel_stack(
        process->kernel_stack_top + PAGE_SIZE
    );
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
    for (unsigned int offset = 1;
         offset <= PROCESS_MAX;
         offset++) {
        int index =
            (from_index + (int)offset) %
            PROCESS_MAX;

        if (process_is_runnable(index)) {
            return index;
        }
    }

    return -1;
}

static void build_initial_context(
    struct process* process
) {
    /* vm_alloc_pages() returns the page base; the stack grows downward,
     * so its initial top is one page above that base. */
    unsigned int* stack =
        (unsigned int*)(unsigned long)
            (process->kernel_stack_top + PAGE_SIZE);

    /*
     * Layout matches:
     *
     *   pushad
     *   iretd frame
     *
     * The lowest address must contain EDI because popad runs first.
     */
    *--stack = USER_DATA_SELECTOR; /* SS */
    *--stack = process->initial_user_esp; /* ESP */
    *--stack = 0x202U; /* EFLAGS, IF=1 */
    *--stack = USER_CODE_SELECTOR; /* CS */
    *--stack = process->entry_point; /* EIP */

    *--stack = 0; /* EAX */
    *--stack = 0; /* ECX */
    *--stack = 0; /* EDX */
    *--stack = 0; /* EBX */
    *--stack = 0; /* original ESP */
    *--stack = 0; /* EBP */
    *--stack = 0; /* ESI */
    *--stack = 0; /* EDI */

    process->saved_esp =
        (unsigned int)(unsigned long)stack;
    process->started = 0;
}

static unsigned int bounded_string_length(
    const char* text,
    unsigned int maximum
) {
    unsigned int length = 0;

    if (!text) {
        return 0;
    }

    while (length < maximum &&
           text[length] != '\0') {
        length++;
    }

    return length;
}

static int process_setup_arguments(
    struct process* process,
    unsigned int argc,
    const char* const* argv
) {
    unsigned int argument_addresses[PROCESS_ARG_MAX];
    unsigned int stack_pointer;
    unsigned int argv_address;
    unsigned int vector_size;

    if (!process ||
        argc == 0U ||
        argc > PROCESS_ARG_MAX ||
        !argv) {
        return 0;
    }

    stack_pointer = USER_STACK_TOP;

    /*
     * Copy argument strings downward from the top of the
     * single user stack page. Keeping argv strings inside
     * the stack page makes their lifetime identical to the
     * process stack itself.
     */
    for (unsigned int i = argc;
         i > 0U;
         i--) {
        const char* argument =
            argv[i - 1U];
        unsigned int length =
            bounded_string_length(
                argument,
                PROCESS_ARG_MAX_LEN
            );

        if (!argument ||
            length >= PROCESS_ARG_MAX_LEN) {
            return 0;
        }

        if (stack_pointer <
            USER_STACK_BASE + length + 1U) {
            return 0;
        }

        stack_pointer -= length + 1U;

        if (!paging_write_user_memory(
                process->cr3,
                stack_pointer,
                argument,
                length + 1U
            )) {
            return 0;
        }

        argument_addresses[i - 1U] =
            stack_pointer;
    }

    stack_pointer &= ~0xFU;

    vector_size =
        (argc + 1U) * sizeof(unsigned int);

    if (stack_pointer <
        USER_STACK_BASE + vector_size + 12U) {
        return 0;
    }

    argv_address =
        stack_pointer - vector_size;

    for (unsigned int i = 0;
         i < argc;
         i++) {
        if (!paging_write_user_memory(
                process->cr3,
                argv_address + i * 4U,
                &argument_addresses[i],
                sizeof(unsigned int)
            )) {
            return 0;
        }
    }

    {
        unsigned int null_pointer = 0;

        if (!paging_write_user_memory(
                process->cr3,
                argv_address + argc * 4U,
                &null_pointer,
                sizeof(unsigned int)
            )) {
            return 0;
        }
    }

    stack_pointer =
        argv_address - 12U;
    stack_pointer &= ~0xFU;

    {
        unsigned int zero = 0;

        if (!paging_write_user_memory(
                process->cr3,
                stack_pointer,
                &argc,
                sizeof(unsigned int)
            ) ||
            !paging_write_user_memory(
                process->cr3,
                stack_pointer + 4U,
                &argv_address,
                sizeof(unsigned int)
            ) ||
            !paging_write_user_memory(
                process->cr3,
                stack_pointer + 8U,
                &zero,
                sizeof(unsigned int)
            )) {
            return 0;
        }
    }

    process->initial_user_esp =
        stack_pointer;

    return 1;
}

void scheduler_init(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        processes[i].pid = 0;
        processes[i].state = PROCESS_UNUSED;
        processes[i].cr3 = VM_ALLOC_FAIL;
        processes[i].entry_point = 0;
        processes[i].user_stack_top =
            USER_STACK_TOP;
        processes[i].initial_user_esp =
            USER_STACK_TOP;
        processes[i].user_heap_break =
            USER_HEAP_BASE;
        processes[i].kernel_stack_top = 0;
        processes[i].kernel_stack_physical =
            VM_ALLOC_FAIL;
        processes[i].saved_esp = 0;
        processes[i].started = 0;
        for (int fd = 0; fd < PROCESS_FD_MAX; fd++) {
            processes[i].files[fd] = 0;
        }
        processes[i].name[0] = '\0';
    }

    current_index = -1;
    next_pid = 1;
    scheduler_active = 0;
}

int process_create_from_image_with_args(
    const char* name,
    const unsigned char* image,
    unsigned int image_size,
    unsigned int argc,
    const char* const* argv
) {
    int slot = -1;
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

    process =
        &processes[slot];

    process->kernel_stack_top =
        vm_alloc_pages(
            1,
            PAGE_WRITABLE
        );

    if (process->kernel_stack_top ==
        VM_ALLOC_FAIL) {
        return -1;
    }

    process->kernel_stack_physical =
        paging_get_physical(
            process->kernel_stack_top
        );

    if (process->kernel_stack_physical ==
        VM_ALLOC_FAIL) {
        vm_free_pages(
            process->kernel_stack_top,
            1
        );
        process->kernel_stack_top = 0;
        return -1;
    }

    process->cr3 =
        paging_create_address_space();

    if (process->cr3 == VM_ALLOC_FAIL) {
        vm_free_pages(
            process->kernel_stack_top,
            1
        );
        process->kernel_stack_top = 0;
        process->kernel_stack_physical =
            VM_ALLOC_FAIL;
        return -1;
    }

    if (!elf_load_user_process_from_image(
            process,
            image,
            image_size
        )) {
        paging_destroy_address_space(
            process->cr3
        );
        vm_free_pages(
            process->kernel_stack_top,
            1
        );
        process->cr3 = VM_ALLOC_FAIL;
        process->kernel_stack_top = 0;
        process->kernel_stack_physical =
            VM_ALLOC_FAIL;
        return -1;
    }

    if (!paging_allocate_user_pages(
            process->cr3,
            USER_STACK_BASE,
            1,
            PAGE_WRITABLE
        )) {
        paging_destroy_address_space(
            process->cr3
        );
        vm_free_pages(
            process->kernel_stack_top,
            1
        );
        process->cr3 = VM_ALLOC_FAIL;
        process->kernel_stack_top = 0;
        process->kernel_stack_physical =
            VM_ALLOC_FAIL;
        return -1;
    }

    process->pid =
        next_pid++;

    if (next_pid == 0) {
        next_pid = 1;
    }

    process->user_stack_top =
        USER_STACK_TOP;

    process->initial_user_esp =
        USER_STACK_TOP;

    process->user_heap_break =
        USER_HEAP_BASE;

    if (!process_setup_arguments(
            process,
            argc,
            argv
        )) {
        paging_destroy_address_space(
            process->cr3
        );
        vm_free_pages(
            process->kernel_stack_top,
            1
        );
        process->cr3 = VM_ALLOC_FAIL;
        process->kernel_stack_top = 0;
        process->kernel_stack_physical =
            VM_ALLOC_FAIL;
        return -1;
    }

    for (int fd = 0; fd < PROCESS_FD_MAX; fd++) {
        process->files[fd] = 0;
    }

    copy_string(
        process->name,
        name
    );

    build_initial_context(process);

    process->state =
        PROCESS_RUNNABLE;

    return (int)process->pid;
}
int process_create_from_image(
    const char* name,
    const unsigned char* image,
    unsigned int image_size
) {
    const char* default_argv[1];

    default_argv[0] = name ? name : "";

    return process_create_from_image_with_args(
        name,
        image,
        image_size,
        1,
        default_argv
    );
}

int process_create(const char* name) {
    const unsigned char* image =
        &user_image_start;
    unsigned int image_size =
        (unsigned int)(
            &user_image_end - &user_image_start
        );

    return process_create_from_image(
        name,
        image,
        image_size
    );
}

int process_run_image_with_args(
    const char* name,
    const unsigned char* image,
    unsigned int image_size,
    unsigned int argc,
    const char* const* argv
) {
    int pid;

    if (!image ||
        image_size == 0U) {
        return -1;
    }

    scheduler_init();

    pid = process_create_from_image_with_args(
        name,
        image,
        image_size,
        argc,
        argv
    );

    if (pid < 0) {
        scheduler_cleanup();
        return -1;
    }

    if (!scheduler_prepare_first()) {
        scheduler_cleanup();
        return -1;
    }

    enter_user_mode(
        scheduler_current_entry(),
        scheduler_current_stack_top()
    );

    scheduler_cleanup();
    return pid;
}

int process_run_image(
    const char* name,
    const unsigned char* image,
    unsigned int image_size
) {
    const char* default_argv[1];

    default_argv[0] = name ? name : "";

    return process_run_image_with_args(
        name,
        image,
        image_size,
        1,
        default_argv
    );
}


int process_exec_image(
    const char* name,
    const unsigned char* image,
    unsigned int image_size
) {
    struct process candidate;
    struct process* process;
    unsigned int old_cr3;
    unsigned int new_cr3;

    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index) ||
        !image ||
        image_size == 0U) {
        return 0;
    }

    process = &processes[current_index];

    new_cr3 =
        paging_create_address_space();

    if (new_cr3 == VM_ALLOC_FAIL) {
        return 0;
    }

    candidate.cr3 = new_cr3;
    candidate.entry_point = 0;
    candidate.user_stack_top = USER_STACK_TOP;

    if (!elf_load_user_process_from_image(
            &candidate,
            image,
            image_size
        )) {
        paging_destroy_address_space(new_cr3);
        return 0;
    }

    if (!paging_allocate_user_pages(
            new_cr3,
            USER_STACK_BASE,
            1,
            PAGE_WRITABLE
        )) {
        paging_destroy_address_space(new_cr3);
        return 0;
    }

    old_cr3 = process->cr3;

    process->cr3 = new_cr3;
    process->entry_point =
        candidate.entry_point;
    process->user_stack_top =
        USER_STACK_TOP;
    process->initial_user_esp =
        USER_STACK_TOP;
    process->user_heap_break =
        USER_HEAP_BASE;

    if (name) {
        copy_string(
            process->name,
            name
        );
    }

    /*
     * Reuse the same PID, kernel stack and file descriptors.
     * Only the user address space and execution context change.
     */
    build_initial_context(process);

    paging_switch_directory(new_cr3);
    /* TSS.esp0 must point one byte past the allocated stack page. */
    syscall_set_kernel_stack(
        process->kernel_stack_top + PAGE_SIZE
    );

    (void)paging_destroy_address_space(old_cr3);

    return 1;
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
            (unsigned int)(unsigned long)
                interrupt_stack;
        processes[current_index].started = 1;
    }

    next =
        find_next_runnable(current_index);

    if (next < 0) {
        if (current_index >= 0 &&
            process_is_runnable(current_index)) {
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

unsigned int scheduler_on_timer(
    unsigned int* interrupt_stack
) {
    int next;

    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index)) {
        return 0;
    }

    /*
     * [9] is CS in the pushad + interrupt frame.
     * Only preempt Ring 3 execution.
     */
    if ((interrupt_stack[9] & 3U) != 3U) {
        return 0;
    }

    processes[current_index].saved_esp =
        (unsigned int)(unsigned long)
            interrupt_stack;
    processes[current_index].started = 1;

    next =
        find_next_runnable(current_index);

    if (next < 0 ||
        next == current_index) {
        return 0;
    }

    current_index = next;
    activate_process(current_index);

    return processes[current_index].saved_esp;
}

unsigned int scheduler_on_syscall(
    unsigned int* interrupt_stack
) {
    if (!scheduler_active ||
        current_index < 0) {
        return 0;
    }

    if ((interrupt_stack[9] & 3U) != 3U) {
        return 0;
    }

    return switch_to_next(
        interrupt_stack,
        processes[current_index].state ==
            PROCESS_RUNNABLE
    );
}

unsigned int scheduler_on_exec(void) {
    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index) ||
        processes[current_index].saved_esp == 0) {
        return 0;
    }

    activate_process(current_index);
    return processes[current_index].saved_esp;
}


int scheduler_current_pid(void) {
    if (!scheduler_active ||
        current_index < 0 ||
        processes[current_index].state ==
            PROCESS_UNUSED) {
        return -1;
    }

    return (int)processes[current_index].pid;
}

unsigned int scheduler_current_entry(void) {
    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index)) {
        return VM_ALLOC_FAIL;
    }

    return processes[current_index].entry_point;
}

unsigned int scheduler_current_stack_top(void) {
    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index)) {
        return VM_ALLOC_FAIL;
    }

    return processes[current_index].initial_user_esp;
}

unsigned int scheduler_current_cr3(void) {
    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index)) {
        return VM_ALLOC_FAIL;
    }

    return processes[current_index].cr3;
}

int process_fd_install(struct vfs_file* file) {
    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index) ||
        !file) {
        return -1;
    }

    for (int fd = 0; fd < PROCESS_FD_MAX; fd++) {
        if (!processes[current_index].files[fd]) {
            processes[current_index].files[fd] = file;
            return fd;
        }
    }

    return -1;
}

struct vfs_file* process_fd_get(int fd) {
    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index) ||
        fd < 0 ||
        fd >= PROCESS_FD_MAX) {
        return 0;
    }

    return processes[current_index].files[fd];
}

int process_fd_close(int fd) {
    struct vfs_file* file;

    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index) ||
        fd < 0 ||
        fd >= PROCESS_FD_MAX) {
        return 0;
    }

    file = processes[current_index].files[fd];

    if (!file) {
        return 0;
    }

    processes[current_index].files[fd] = 0;
    return vfs_close(file);
}

int process_sbrk(
    unsigned int increment,
    unsigned int* old_break
) {
    struct process* process;
    unsigned int old_value;
    unsigned int new_value;
    unsigned int old_pages_end;
    unsigned int new_pages_end;
    unsigned int page_count;

    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index) ||
        !old_break) {
        return 0;
    }

    process =
        &processes[current_index];

    old_value = process->user_heap_break;

    if (increment >
        USER_HEAP_END - old_value) {
        return 0;
    }

    new_value =
        old_value + increment;

    old_pages_end =
        (old_value + PAGE_SIZE - 1U) &
        0xFFFFF000U;

    new_pages_end =
        (new_value + PAGE_SIZE - 1U) &
        0xFFFFF000U;

    if (new_pages_end > old_pages_end) {
        page_count =
            (new_pages_end - old_pages_end) /
            PAGE_SIZE;

        if (!paging_allocate_user_pages(
                process->cr3,
                old_pages_end,
                page_count,
                PAGE_WRITABLE
            )) {
            return 0;
        }
    }

    process->user_heap_break = new_value;
    *old_break = old_value;

    return 1;
}

int process_exit_current(void) {
    if (!scheduler_active ||
        current_index < 0 ||
        !process_is_runnable(current_index)) {
        return 0;
    }

    for (int fd = 0; fd < PROCESS_FD_MAX; fd++) {
        if (processes[current_index].files[fd]) {
            vfs_close(
                processes[current_index].files[fd]
            );
            processes[current_index].files[fd] = 0;
        }
    }

    processes[current_index].state =
        PROCESS_TERMINATED;

    return 1;
}

void scheduler_print_processes(void) {
    print_string(
        "PID   STATE        CR3         NAME\n",
        0x0A
    );

    for (int i = 0;
         i < PROCESS_MAX;
         i++) {
        struct process* process =
            &processes[i];

        if (process->state ==
            PROCESS_UNUSED) {
            continue;
        }

        print_uint(
            process->pid,
            0x0F
        );
        print_string(
            "     ",
            0x07
        );

        if (process->state ==
            PROCESS_RUNNABLE) {
            print_string(
                "RUNNABLE     ",
                0x0E
            );
        } else {
            print_string(
                "TERMINATED   ",
                0x08
            );
        }

        print_uint(
            process->cr3,
            0x0F
        );
        print_string(
            "  ",
            0x07
        );

        print_string(
            process->name,
            0x0F
        );

        if (i == current_index) {
            print_string(
                "  <current>",
                0x0B
            );
        }

        print_char('\n', 0x07);
    }
}

void scheduler_cleanup(void) {
    paging_switch_directory(
        paging_get_kernel_directory()
    );

    for (int i = 0;
         i < PROCESS_MAX;
         i++) {
        struct process* process =
            &processes[i];

        if (process->state ==
            PROCESS_UNUSED) {
            continue;
        }

        if (process->cr3 != VM_ALLOC_FAIL) {
            paging_destroy_address_space(
                process->cr3
            );
        }

        if (process->kernel_stack_top !=
            VM_ALLOC_FAIL &&
            process->kernel_stack_top != 0) {
            vm_free_pages(
                process->kernel_stack_top,
                1
            );
        }

        process->pid = 0;
        process->state =
            PROCESS_UNUSED;
        process->cr3 = VM_ALLOC_FAIL;
        process->entry_point = 0;
        process->user_stack_top =
            USER_STACK_TOP;
        process->initial_user_esp =
            USER_STACK_TOP;
        process->user_heap_break =
            USER_HEAP_BASE;
        process->kernel_stack_physical =
            VM_ALLOC_FAIL;
        process->kernel_stack_top = 0;
        process->saved_esp = 0;
        process->started = 0;
        for (int fd = 0; fd < PROCESS_FD_MAX; fd++) {
            process->files[fd] = 0;
        }
        process->name[0] = '\0';
    }

    current_index = -1;
    scheduler_active = 0;

    syscall_set_kernel_stack(
        (unsigned int)(unsigned long)
            &stack_top
    );
}
