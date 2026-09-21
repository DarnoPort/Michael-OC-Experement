#include "syscalls.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "vfs.h"

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

static int copy_user_path(
    unsigned int directory,
    unsigned int address,
    char* path,
    unsigned int path_size
) {
    if (!path || path_size < 2U) {
        return 0;
    }

    for (unsigned int i = 0; i < path_size - 1U; i++) {
        unsigned char character;

        if (address > 0xFFFFFFFFU - i ||
            !paging_read_user_memory(
                directory,
                address + i,
                &character,
                1
            )) {
            return 0;
        }

        path[i] = (char)character;

        if (character == '\0') {
            return 1;
        }
    }

    path[path_size - 1U] = '\0';
    return 0;
}

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

static int syscall_load_vfs_image(
    const char* path,
    unsigned char** image,
    unsigned int* image_size
) {
    struct vfs_node* node;
    struct vfs_file* file;
    unsigned char* buffer;
    unsigned int size;
    unsigned int total = 0;

    if (!path || !image || !image_size) {
        return 0;
    }

    node = vfs_lookup(path);

    if (!node ||
        vfs_node_is_directory(node)) {
        return 0;
    }

    size = vfs_node_size(node);

    if (size == 0U ||
        size > VFS_MAX_FILE_SIZE) {
        return 0;
    }

    buffer =
        (unsigned char*)malloc(size);

    if (!buffer) {
        return 0;
    }

    file =
        vfs_open(
            path,
            VFS_O_READ
        );

    if (!file) {
        free(buffer);
        return 0;
    }

    while (total < size) {
        unsigned int remaining =
            size - total;
        unsigned int chunk =
            remaining > 4096U
                ? 4096U
                : remaining;
        int result =
            vfs_read(
                file,
                buffer + total,
                chunk
            );

        if (result <= 0) {
            vfs_close(file);
            free(buffer);
            return 0;
        }

        total += (unsigned int)result;

        if ((unsigned int)result < chunk &&
            total < size) {
            vfs_close(file);
            free(buffer);
            return 0;
        }
    }

    vfs_close(file);

    *image = buffer;
    *image_size = size;
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

    if (registers->eax == SYS_OPEN) {
        char path[VFS_PATH_MAX];
        struct vfs_file* file;
        unsigned int flags;
        int fd;

        if (!copy_user_path(
                scheduler_current_cr3(),
                registers->ebx,
                path,
                sizeof(path)
            )) {
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        flags = registers->ecx &
            (VFS_O_READ |
             VFS_O_WRITE |
             VFS_O_CREATE |
             VFS_O_TRUNC);

        if (!(flags & (VFS_O_READ | VFS_O_WRITE))) {
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        file = vfs_open(path, flags);

        if (!file) {
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        fd = process_fd_install(file);

        if (fd < 0) {
            vfs_close(file);
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        registers->eax = (unsigned int)fd;
        return 0;
    }

    if (registers->eax == SYS_FILE_READ) {
        struct vfs_file* file =
            process_fd_get((int)registers->ebx);
        unsigned int directory =
            scheduler_current_cr3();
        unsigned int remaining =
            registers->edx;
        unsigned int destination =
            registers->ecx;
        unsigned char buffer[256];
        unsigned int total = 0;

        if (!file ||
            remaining == 0 ||
            remaining > 4096U ||
            !paging_user_range_valid_in_directory(
                directory,
                destination,
                remaining,
                1
            )) {
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        while (remaining > 0) {
            unsigned int chunk =
                remaining > sizeof(buffer)
                    ? sizeof(buffer)
                    : remaining;
            int result =
                vfs_read(
                    file,
                    buffer,
                    chunk
                );

            if (result < 0) {
                if (total == 0) {
                    registers->eax = 0xFFFFFFFFU;
                } else {
                    registers->eax = total;
                }
                return 0;
            }

            if (result == 0) {
                break;
            }

            if (!paging_write_user_memory(
                    directory,
                    destination + total,
                    buffer,
                    (unsigned int)result
                )) {
                registers->eax =
                    total == 0
                        ? 0xFFFFFFFFU
                        : total;
                return 0;
            }

            total += (unsigned int)result;
            remaining -= (unsigned int)result;

            if ((unsigned int)result < chunk) {
                break;
            }
        }

        registers->eax = total;
        return 0;
    }

    if (registers->eax == SYS_FILE_WRITE) {
        struct vfs_file* file =
            process_fd_get((int)registers->ebx);
        unsigned int directory =
            scheduler_current_cr3();
        unsigned int remaining =
            registers->edx;
        unsigned int source =
            registers->ecx;
        unsigned char buffer[256];
        unsigned int total = 0;

        if (!file ||
            remaining == 0 ||
            remaining > 4096U ||
            !paging_user_range_valid_in_directory(
                directory,
                source,
                remaining,
                0
            )) {
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        while (remaining > 0) {
            unsigned int chunk =
                remaining > sizeof(buffer)
                    ? sizeof(buffer)
                    : remaining;

            if (!paging_read_user_memory(
                    directory,
                    source + total,
                    buffer,
                    chunk
                )) {
                registers->eax =
                    total == 0
                        ? 0xFFFFFFFFU
                        : total;
                return 0;
            }

            {
                int result =
                    vfs_write(
                        file,
                        buffer,
                        chunk
                    );

                if (result < 0) {
                    registers->eax =
                        total == 0
                            ? 0xFFFFFFFFU
                            : total;
                    return 0;
                }

                total += (unsigned int)result;
                remaining -= (unsigned int)result;

                if ((unsigned int)result < chunk) {
                    break;
                }
            }
        }

        registers->eax = total;
        return 0;
    }

    if (registers->eax == SYS_CLOSE) {
        registers->eax =
            process_fd_close((int)registers->ebx)
                ? 0U
                : 0xFFFFFFFFU;
        return 0;
    }

    if (registers->eax == SYS_EXEC) {
        char path[VFS_PATH_MAX];
        unsigned char* image;
        unsigned int image_size;

        if (!copy_user_path(
                scheduler_current_cr3(),
                registers->ebx,
                path,
                sizeof(path)
            )) {
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        if (!syscall_load_vfs_image(
                path,
                &image,
                &image_size
            )) {
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        if (!process_exec_image(
                path,
                image,
                image_size
            )) {
            free(image);
            registers->eax = 0xFFFFFFFFU;
            return 0;
        }

        free(image);
        registers->eax = 0;
        return 3;
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


static int verify_worker_file(
    const char* path,
    char expected_pid
) {
    unsigned char buffer[2];
    struct vfs_file* file;
    int result;
    int ok = 1;

    file =
        vfs_open(
            path,
            VFS_O_READ
        );

    if (!file) {
        return 0;
    }

    result =
        vfs_read(
            file,
            buffer,
            sizeof(buffer)
        );

    if (result != 2 ||
        buffer[0] != (unsigned char)expected_pid ||
        buffer[1] != 10) {
        ok = 0;
    }

    vfs_close(file);
    return ok;
}

void syscall_run_test(void) {
    int pid_a;
    int pid_b;

    scheduler_init();

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

    if (pid_b >= 0 &&
        verify_worker_file(
            "/worker1.txt",
            '1'
        ) &&
        verify_worker_file(
            "/worker2.txt",
            '2'
        )) {
        print_string(
            "usertest: user DiskFS syscalls PASS. Files survive reboot.\n",
            0x0A
        );
    } else {
        print_string(
            "usertest: user DiskFS syscall verification FAILED.\n",
            0x0C
        );
    }
}
