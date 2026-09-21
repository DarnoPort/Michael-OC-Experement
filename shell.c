#include "shell.h"
#include "vfs.h"
#include "diskfs.h"
#include "terminal.h"
#include "process.h"
#include "memory.h"
#include "version.h"

extern void print_char(char c, unsigned char color);
extern void print_string(const char* str, unsigned char color);
extern const unsigned char user_image_start;
extern const unsigned char user_image_end;
extern const unsigned char user_exec_image_start;
extern const unsigned char user_exec_image_end;
extern const unsigned char user_args_image_start;
extern const unsigned char user_args_image_end;

#define SHELL_FD_MAX 8

static struct vfs_file* shell_files[SHELL_FD_MAX];
static char shell_cwd[VFS_PATH_MAX] = "/";

static void shell_update_prompt(void) {
    char prompt[TERMINAL_PROMPT_MAX];
    unsigned int position = 0;

    prompt[position++] = 'C';
    prompt[position++] = ':';

    for (unsigned int i = 0;
         shell_cwd[i] != '\0';
         i++) {
        if (position + 4U >= TERMINAL_PROMPT_MAX) {
            prompt[position++] = '.';
            prompt[position++] = '.';
            prompt[position++] = '.';
            break;
        }

        prompt[position++] =
            shell_cwd[i] == '/'
                ? '\\'
                : shell_cwd[i];
    }

    if (position + 2U >= TERMINAL_PROMPT_MAX) {
        position = TERMINAL_PROMPT_MAX - 3U;
    }

    prompt[position++] = '>';
    prompt[position] = '\0';

    terminal_set_prompt(prompt);
}

static unsigned int shell_length(const char* text) {
    unsigned int length = 0;

    if (!text) {
        return 0;
    }

    while (text[length] != '\0') {
        length++;
        if (length >= VFS_PATH_MAX) {
            break;
        }
    }

    return length;
}

static void shell_copy(
    char* destination,
    const char* source,
    unsigned int size
) {
    unsigned int i = 0;

    if (!destination || size == 0) {
        return;
    }

    if (!source) {
        destination[0] = '\0';
        return;
    }

    while (i + 1U < size &&
           source[i] != '\0') {
        destination[i] = source[i];
        i++;
    }

    destination[i] = '\0';
}


static void shell_print_uint(
    unsigned int value
) {
    char digits[10];
    int count = 0;

    if (value == 0) {
        print_char('0', 0x0F);
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
            0x0F
        );
    }
}

static const char* skip_spaces(
    const char* text
) {
    while (text &&
           (*text == ' ' || *text == '\t')) {
        text++;
    }

    return text;
}

static int command_args(
    const char* command,
    const char* keyword,
    const char** args
) {
    unsigned int length;
    const char* cursor;

    if (!command || !keyword || !args) {
        return 0;
    }

    length = 0;
    while (keyword[length] != '\0') {
        length++;
    }

    cursor = command;

    for (unsigned int i = 0;
         i < length;
         i++) {
        if (cursor[i] != keyword[i]) {
            return 0;
        }
    }

    if (cursor[length] != '\0' &&
        cursor[length] != ' ' &&
        cursor[length] != '\t') {
        return 0;
    }

    *args = cursor + length;
    return 1;
}

static int next_token(
    const char** cursor,
    char* token,
    unsigned int token_size
) {
    const char* text;
    unsigned int length = 0;

    if (!cursor ||
        !token ||
        token_size < 2U) {
        return 0;
    }

    text = skip_spaces(*cursor);

    if (!text || *text == '\0') {
        return 0;
    }

    if (*text == '"') {
        text++;

        while (text[length] != '\0' &&
               text[length] != '"') {
            if (length + 1U >= token_size) {
                return 0;
            }

            token[length] =
                text[length];
            length++;
        }

        if (text[length] != '"') {
            return 0;
        }

        token[length] = '\0';
        *cursor = text + length + 1U;
        return 1;
    }

    while (text[length] != '\0' &&
           text[length] != ' ' &&
           text[length] != '\t') {
        if (length + 1U >= token_size) {
            return 0;
        }

        token[length] =
            text[length];
        length++;
    }

    token[length] = '\0';
    *cursor = text + length;
    return 1;
}

static int parse_uint(
    const char* text,
    unsigned int* value
) {
    unsigned int result = 0;
    unsigned int digits = 0;

    if (!text || !value) {
        return 0;
    }

    text = skip_spaces(text);

    while (*text >= '0' && *text <= '9') {
        unsigned int digit =
            (unsigned int)(*text - '0');

        if (result >
            (0xFFFFFFFFU - digit) / 10U) {
            return 0;
        }

        result =
            result * 10U + digit;
        digits++;
        text++;
    }

    if (digits == 0 ||
        *skip_spaces(text) != '\0') {
        return 0;
    }

    *value = result;
    return 1;
}

static int make_path(
    const char* input,
    char* output
) {
    unsigned int input_length;
    unsigned int cwd_length;
    unsigned int position;

    input = skip_spaces(input);

    if (!input || *input == '\0' ||
        !output) {
        return 0;
    }

    input_length = shell_length(input);

    if (input_length >= VFS_PATH_MAX) {
        return 0;
    }

    if (input[0] == '/') {
        shell_copy(
            output,
            input,
            VFS_PATH_MAX
        );
        return 1;
    }

    cwd_length =
        shell_length(shell_cwd);

    if (cwd_length == 1U &&
        shell_cwd[0] == '/') {
        if (input_length + 2U >
            VFS_PATH_MAX) {
            return 0;
        }

        output[0] = '/';

        for (unsigned int i = 0;
             i < input_length;
             i++) {
            output[i + 1U] =
                input[i];
        }

        output[input_length + 1U] =
            '\0';
        return 1;
    }

    if (cwd_length + input_length + 2U >
        VFS_PATH_MAX) {
        return 0;
    }

    position = 0;

    for (unsigned int i = 0;
         i < cwd_length;
         i++) {
        output[position++] =
            shell_cwd[i];
    }

    output[position++] = '/';

    for (unsigned int i = 0;
         i < input_length;
         i++) {
        output[position++] =
            input[i];
    }

    output[position] = '\0';
    return 1;
}

static void print_error(
    const char* command,
    const char* message
) {
    print_string(command, 0x0C);
    print_string(message, 0x0C);
    print_char('\n', 0x07);
}

static void command_ls(const char* args) {
    char path[VFS_PATH_MAX];
    struct vfs_node* node;

    args = skip_spaces(args);

    if (!args || *args == '\0') {
        shell_copy(
            path,
            shell_cwd,
            sizeof(path)
        );
    } else if (!make_path(args, path)) {
        print_error("ls: ", "invalid path.");
        return;
    }

    node = vfs_lookup(path);

    if (!node) {
        print_error("ls: ", "path not found.");
        return;
    }

    if (!vfs_node_is_directory(node)) {
        print_string(
            vfs_node_name(node),
            0x0F
        );
        print_string("  ", 0x07);
        print_string("FILE  ", 0x0E);

        {
            unsigned int size =
                vfs_node_size(node);

            if (size == 0) {
                print_char('0', 0x0F);
            } else {
                char digits[10];
                int count = 0;

                while (size > 0) {
                    digits[count++] =
                        (char)('0' + size % 10U);
                    size /= 10U;
                }

                while (count > 0) {
                    print_char(
                        digits[--count],
                        0x0F
                    );
                }
            }
        }

        print_string(" bytes\n", 0x07);
        return;
    }

    {
        struct vfs_node* child =
            vfs_node_first_child(node);

        if (!child) {
            print_string(
                "(empty)\n",
                0x08
            );
            return;
        }

        while (child) {
            if (vfs_node_is_directory(child)) {
                print_string(
                    "[DIR ] ",
                    0x0B
                );
            } else {
                print_string(
                    "[FILE] ",
                    0x0E
                );
            }

            print_string(
                vfs_node_name(child),
                0x0F
            );

            if (!vfs_node_is_directory(child)) {
                unsigned int size =
                    vfs_node_size(child);

                print_string("  ", 0x07);
                if (size == 0) {
                    print_char('0', 0x08);
                } else {
                    char digits[10];
                    int count = 0;

                    while (size > 0) {
                        digits[count++] =
                            (char)('0' + size % 10U);
                        size /= 10U;
                    }

                    while (count > 0) {
                        print_char(
                            digits[--count],
                            0x08
                        );
                    }
                }

                print_string(
                    " bytes",
                    0x07
                );
            }

            print_char('\n', 0x07);
            child =
                vfs_node_next_sibling(child);
        }
    }
}

static void command_cat(const char* args) {
    char path[VFS_PATH_MAX];
    unsigned char buffer[128];
    struct vfs_file* file;
    int result;

    if (!make_path(args, path)) {
        print_error("cat: ", "invalid path.");
        return;
    }

    file =
        vfs_open(
            path,
            VFS_O_READ
        );

    if (!file) {
        print_error("cat: ", "cannot open file.");
        return;
    }

    for (;;) {
        result =
            vfs_read(
                file,
                buffer,
                sizeof(buffer)
            );

        if (result < 0) {
            print_error("cat: ", "read error.");
            break;
        }

        if (result == 0) {
            break;
        }

        for (int i = 0;
             i < result;
             i++) {
            print_char(
                (char)buffer[i],
                0x0F
            );
        }
    }

    if (vfs_file_offset(file) > 0) {
        print_char('\n', 0x07);
    }

    vfs_close(file);
}

static void command_write(const char* args) {
    const char* cursor =
        skip_spaces(args);
    char relative_path[VFS_PATH_MAX];
    char path[VFS_PATH_MAX];
    struct vfs_file* file;
    const char* data;
    unsigned int length;
    int written;

    if (!next_token(
            &cursor,
            relative_path,
            sizeof(relative_path)
        ) ||
        !make_path(
            relative_path,
            path
        )) {
        print_error(
            "write: ",
            "usage: write <path> <text>"
        );
        return;
    }

    data = skip_spaces(cursor);

    if (!data || *data == '\0') {
        print_error(
            "write: ",
            "text is required."
        );
        return;
    }

    length = shell_length(data);

    if (length >= 2U &&
        data[0] == '"' &&
        data[length - 1U] == '"') {
        data++;
        length -= 2U;
    }

    file =
        vfs_open(
            path,
            VFS_O_WRITE |
            VFS_O_CREATE |
            VFS_O_TRUNC
        );

    if (!file) {
        print_error(
            "write: ",
            "cannot open file."
        );
        return;
    }

    written =
        vfs_write(
            file,
            data,
            length
        );

    vfs_close(file);

    if (written < 0 ||
        (unsigned int)written != length) {
        print_error(
            "write: ",
            "write failed."
        );
    }
}

static void command_open(const char* args) {
    char path[VFS_PATH_MAX];
    struct vfs_file* file;
    int fd;

    if (!make_path(args, path)) {
        print_error(
            "open: ",
            "usage: open <path>"
        );
        return;
    }

    for (fd = 0;
         fd < SHELL_FD_MAX;
         fd++) {
        if (!shell_files[fd]) {
            break;
        }
    }

    if (fd >= SHELL_FD_MAX) {
        print_error(
            "open: ",
            "descriptor table is full."
        );
        return;
    }

    file =
        vfs_open(
            path,
            VFS_O_READ |
            VFS_O_WRITE
        );

    if (!file) {
        print_error(
            "open: ",
            "cannot open file."
        );
        return;
    }

    shell_files[fd] = file;

    print_string(
        "fd = ",
        0x0E
    );

    if (fd == 0) {
        print_char('0', 0x0F);
    } else {
        print_char(
            (char)('0' + fd),
            0x0F
        );
    }

    print_char('\n', 0x07);
}

static void command_read(const char* args) {
    const char* cursor =
        skip_spaces(args);
    char fd_text[16];
    unsigned int fd;
    unsigned int length = 128U;
    struct vfs_file* file;
    unsigned char buffer[128];
    int result;

    if (!next_token(
            &cursor,
            fd_text,
            sizeof(fd_text)
        ) ||
        !parse_uint(
            fd_text,
            &fd
        ) ||
        fd >= SHELL_FD_MAX ||
        !shell_files[fd]) {
        print_error(
            "read: ",
            "invalid file descriptor."
        );
        return;
    }

    cursor = skip_spaces(cursor);

    if (cursor && *cursor != '\0' &&
        (!parse_uint(cursor, &length) ||
         length == 0 ||
         length > sizeof(buffer))) {
        print_error(
            "read: ",
            "invalid length."
        );
        return;
    }

    file = shell_files[fd];

    result =
        vfs_read(
            file,
            buffer,
            length
        );

    if (result < 0) {
        print_error(
            "read: ",
            "read failed."
        );
        return;
    }

    for (int i = 0;
         i < result;
         i++) {
        print_char(
            (char)buffer[i],
            0x0F
        );
    }

    print_char('\n', 0x07);
}

static void command_close(const char* args) {
    unsigned int fd;

    if (!parse_uint(args, &fd) ||
        fd >= SHELL_FD_MAX ||
        !shell_files[fd] ||
        !vfs_close(shell_files[fd])) {
        print_error(
            "close: ",
            "invalid file descriptor."
        );
        return;
    }

    shell_files[fd] = 0;
}


static int command_diskinfo(void) {
    unsigned int total;
    unsigned int free_sectors;
    unsigned int used_inodes;

    if (!diskfs_get_stats(
            &total,
            &free_sectors,
            &used_inodes
        )) {
        print_error(
            "diskinfo: ",
            "filesystem statistics unavailable."
        );
        return 0;
    }

    print_string(
        "DiskFS sectors: ",
        0x0E
    );
    shell_print_uint(total);
    print_string(
        "\nFree data sectors: ",
        0x0E
    );
    shell_print_uint(free_sectors);
    print_string(
        "\nUsed inodes: ",
        0x0E
    );
    shell_print_uint(used_inodes);
    print_string(
        "\n",
        0x07
    );

    return 1;
}

static int command_fstest(void) {
    static const char message[] =
        "Hello Michael OS Phase 18!";
    unsigned char buffer[
        sizeof(message)
    ];
    struct vfs_file* file;
    int result;
    int ok = 1;

    if (!vfs_lookup("/phase15")) {
        if (!vfs_mkdir("/phase15")) {
            print_error(
                "fstest: ",
                "mkdir failed."
            );
            return 0;
        }
    }

    if (!vfs_lookup("/phase15/hello.txt")) {
        if (!vfs_create_file(
                "/phase15/hello.txt"
            )) {
            print_error(
                "fstest: ",
                "file creation failed."
            );
            return 0;
        }
    }

    file =
        vfs_open(
            "/phase15/hello.txt",
            VFS_O_READ |
            VFS_O_WRITE |
            VFS_O_TRUNC
        );

    if (!file) {
        print_error(
            "fstest: ",
            "open failed."
        );
        return 0;
    }

    result =
        vfs_write(
            file,
            message,
            sizeof(message) - 1U
        );

    if (result !=
        (int)(sizeof(message) - 1U)) {
        ok = 0;
    }

    if (!vfs_seek(file, 0)) {
        ok = 0;
    }

    result =
        vfs_read(
            file,
            buffer,
            sizeof(message) - 1U
        );

    if (result !=
        (int)(sizeof(message) - 1U)) {
        ok = 0;
    } else {
        for (unsigned int i = 0;
             i < sizeof(message) - 1U;
             i++) {
            if (buffer[i] != message[i]) {
                ok = 0;
                break;
            }
        }
    }

    vfs_close(file);

    if (ok) {
        print_string(
            "fstest: PASS (DiskFS mkdir/create/open/write/read/close)\n",
            0x0A
        );
    } else {
        print_error(
            "fstest: ",
            "FAILED."
        );
    }

    return ok;
}

static int shell_read_file_image(
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
        print_error(
            "run: ",
            "executable file not found."
        );
        return 0;
    }

    size = vfs_node_size(node);

    if (size == 0U ||
        size > VFS_MAX_FILE_SIZE) {
        print_error(
            "run: ",
            "invalid executable size."
        );
        return 0;
    }

    buffer =
        (unsigned char*)malloc(size);

    if (!buffer) {
        print_error(
            "run: ",
            "not enough kernel memory."
        );
        return 0;
    }

    file =
        vfs_open(
            path,
            VFS_O_READ
        );

    if (!file) {
        free(buffer);
        print_error(
            "run: ",
            "cannot open executable."
        );
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
            print_error(
                "run: ",
                "cannot read executable."
            );
            return 0;
        }

        total += (unsigned int)result;

        if ((unsigned int)result < chunk &&
            total < size) {
            vfs_close(file);
            free(buffer);
            print_error(
                "run: ",
                "executable read was truncated."
            );
            return 0;
        }
    }

    vfs_close(file);

    *image = buffer;
    *image_size = size;
    return 1;
}

static void command_run(
    const char* args
) {
    const char* cursor =
        skip_spaces(args);
    char argument[PROCESS_ARG_MAX_LEN];
    char path[VFS_PATH_MAX];
    char argv_storage[
        PROCESS_ARG_MAX
    ][PROCESS_ARG_MAX_LEN];
    const char* argv[
        PROCESS_ARG_MAX
    ];
    unsigned int argc = 0;
    unsigned char* image;
    unsigned int image_size;
    struct vfs_node* node;
    int pid;

    if (!next_token(
            &cursor,
            argument,
            sizeof(argument)
        )) {
        print_error(
            "run: ",
            "usage: run <path> [args...]"
        );
        return;
    }

    if (!make_path(argument, path)) {
        print_error(
            "run: ",
            "invalid executable path."
        );
        return;
    }

    shell_copy(
        argv_storage[argc],
        argument,
        sizeof(argv_storage[argc])
    );
    argv[argc] = argv_storage[argc];
    argc++;

    while (*skip_spaces(cursor) != '\0') {
        if (argc >= PROCESS_ARG_MAX) {
            print_error(
                "run: ",
                "too many arguments (maximum is 7 plus argv[0])."
            );
            return;
        }

        if (!next_token(
                &cursor,
                argv_storage[argc],
                sizeof(argv_storage[argc])
            )) {
            print_error(
                "run: ",
                "invalid argument quoting."
            );
            return;
        }

        argv[argc] = argv_storage[argc];
        argc++;
    }

    node = vfs_lookup(path);

    if (!node ||
        vfs_node_is_directory(node)) {
        print_error(
            "run: ",
            "executable file not found."
        );
        return;
    }

    if (!shell_read_file_image(
            path,
            &image,
            &image_size
        )) {
        return;
    }

    print_string(
        "[run] loading ",
        0x0E
    );
    print_string(
        path,
        0x0F
    );
    print_string(
        " into Ring 3...",
        0x0E
    );

    if (argc > 1U) {
        print_string(
            " with ",
            0x07
        );
        print_uint(
            argc - 1U,
            0x0F
        );
        print_string(
            " arg(s)",
            0x07
        );
    }

    print_char('\n', 0x07);

    pid = process_run_image_with_args(
        vfs_node_name(node),
        image,
        image_size,
        argc,
        argv
    );

    free(image);

    if (pid < 0) {
        print_error(
            "run: ",
            "ELF loading, argument setup, or process startup failed."
        );
        return;
    }

    print_string(
        "[run] process ",
        0x0E
    );
    print_uint(
        (unsigned int)pid,
        0x0F
    );
    print_string(
        " exited.\n",
        0x0E
    );
}



static int command_install_demo(void) {
    const unsigned char* image =
        &user_image_start;
    unsigned int image_size =
        (unsigned int)(
            &user_image_end -
            &user_image_start
        );
    struct vfs_file* file;
    unsigned int total = 0;

    if (!vfs_lookup("/bin")) {
        if (!vfs_mkdir("/bin")) {
            print_error(
                "install-demo: ",
                "cannot create /bin."
            );
            return 0;
        }
    }

    if (vfs_lookup("/bin/demo.elf")) {
        print_error(
            "install-demo: ",
            "/bin/demo.elf already exists."
        );
        return 0;
    }

    file =
        vfs_open(
            "/bin/demo.elf",
            VFS_O_WRITE |
            VFS_O_CREATE |
            VFS_O_TRUNC
        );

    if (!file) {
        print_error(
            "install-demo: ",
            "cannot create executable."
        );
        return 0;
    }

    while (total < image_size) {
        unsigned int remaining =
            image_size - total;
        unsigned int chunk =
            remaining > 4096U
                ? 4096U
                : remaining;
        int written =
            vfs_write(
                file,
                image + total,
                chunk
            );

        if (written <= 0 ||
            (unsigned int)written > chunk) {
            vfs_close(file);
            (void)vfs_remove("/bin/demo.elf");
            print_error(
                "install-demo: ",
                "failed while writing executable."
            );
            return 0;
        }

        total += (unsigned int)written;

        if ((unsigned int)written < chunk &&
            total < image_size) {
            vfs_close(file);
            (void)vfs_remove("/bin/demo.elf");
            print_error(
                "install-demo: ",
                "executable write was truncated."
            );
            return 0;
        }
    }

    vfs_close(file);

    print_string(
        "Installed /bin/demo.elf (",
        0x0A
    );
    print_uint(image_size, 0x0F);
    print_string(
        " bytes).\n",
        0x0A
    );
    return 1;
}

static int command_layout(
    const char* args
) {
    const char* value =
        skip_spaces(args);

    if (!value || *value == '\0') {
        print_string(
            "Keyboard layout: ",
            0x0E
        );
        print_string(
            terminal_layout_name(),
            0x0F
        );
        print_string(
            "  (Alt+Shift to switch)\n",
            0x07
        );
        return 1;
    }

    if (value[0] == 'e' &&
        value[1] == 'n' &&
        value[2] == '\0') {
        terminal_set_layout(0);
        print_string(
            "Keyboard layout: EN\n",
            0x0A
        );
        return 1;
    }

    if (value[0] == 'r' &&
        value[1] == 'u' &&
        value[2] == '\0') {
        terminal_set_layout(1);
        print_string(
            "Keyboard layout: RU\n",
            0x0A
        );
        return 1;
    }

    print_error(
        "layout: ",
        "usage: layout [en|ru]"
    );
    return 1;
}

static int command_install_exec_test(void) {
    const unsigned char* image =
        &user_exec_image_start;
    unsigned int image_size =
        (unsigned int)(
            &user_exec_image_end -
            &user_exec_image_start
        );
    struct vfs_file* file;
    unsigned int total = 0;

    if (!vfs_lookup("/bin")) {
        if (!vfs_mkdir("/bin")) {
            print_error(
                "install-exec-test: ",
                "cannot create /bin."
            );
            return 0;
        }
    }

    if (vfs_lookup("/bin/exec-test.elf")) {
        print_error(
            "install-exec-test: ",
            "/bin/exec-test.elf already exists."
        );
        return 0;
    }

    file =
        vfs_open(
            "/bin/exec-test.elf",
            VFS_O_WRITE |
            VFS_O_CREATE |
            VFS_O_TRUNC
        );

    if (!file) {
        print_error(
            "install-exec-test: ",
            "cannot create executable."
        );
        return 0;
    }

    while (total < image_size) {
        unsigned int remaining =
            image_size - total;
        unsigned int chunk =
            remaining > 4096U
                ? 4096U
                : remaining;
        int written =
            vfs_write(
                file,
                image + total,
                chunk
            );

        if (written <= 0 ||
            (unsigned int)written > chunk) {
            vfs_close(file);
            (void)vfs_remove(
                "/bin/exec-test.elf"
            );
            print_error(
                "install-exec-test: ",
                "failed while writing executable."
            );
            return 0;
        }

        total += (unsigned int)written;

        if ((unsigned int)written < chunk &&
            total < image_size) {
            vfs_close(file);
            (void)vfs_remove(
                "/bin/exec-test.elf"
            );
            print_error(
                "install-exec-test: ",
                "executable write was truncated."
            );
            return 0;
        }
    }

    vfs_close(file);

    print_string(
        "Installed /bin/exec-test.elf (",
        0x0A
    );
    print_uint(image_size, 0x0F);
    print_string(
        " bytes).\n",
        0x0A
    );
    return 1;
}

static int command_install_args_test(void) {
    const unsigned char* image =
        &user_args_image_start;
    unsigned int image_size =
        (unsigned int)(
            &user_args_image_end -
            &user_args_image_start
        );
    struct vfs_file* file;
    unsigned int total = 0;

    if (!vfs_lookup("/bin")) {
        if (!vfs_mkdir("/bin")) {
            print_error(
                "install-args-test: ",
                "cannot create /bin."
            );
            return 0;
        }
    }

    if (vfs_lookup("/bin/args-test.elf")) {
        print_error(
            "install-args-test: ",
            "/bin/args-test.elf already exists."
        );
        return 0;
    }

    file =
        vfs_open(
            "/bin/args-test.elf",
            VFS_O_WRITE |
            VFS_O_CREATE |
            VFS_O_TRUNC
        );

    if (!file) {
        print_error(
            "install-args-test: ",
            "cannot create executable."
        );
        return 0;
    }

    while (total < image_size) {
        unsigned int remaining =
            image_size - total;
        unsigned int chunk =
            remaining > 4096U
                ? 4096U
                : remaining;
        int written =
            vfs_write(
                file,
                image + total,
                chunk
            );

        if (written <= 0 ||
            (unsigned int)written > chunk) {
            vfs_close(file);
            (void)vfs_remove("/bin/args-test.elf");
            print_error(
                "install-args-test: ",
                "failed while writing executable."
            );
            return 0;
        }

        total += (unsigned int)written;

        if ((unsigned int)written < chunk &&
            total < image_size) {
            vfs_close(file);
            (void)vfs_remove("/bin/args-test.elf");
            print_error(
                "install-args-test: ",
                "executable write was truncated."
            );
            return 0;
        }
    }

    vfs_close(file);

    print_string(
        "Installed /bin/args-test.elf (",
        0x0A
    );
    print_uint(
        image_size,
        0x0F
    );
    print_string(
        " bytes).\n",
        0x0A
    );
    return 1;
}

int shell_init(void) {
    for (int fd = 0;
         fd < SHELL_FD_MAX;
         fd++) {
        shell_files[fd] = 0;
    }

    shell_cwd[0] = '/';
    shell_cwd[1] = '\0';

    shell_update_prompt();

    return 1;
}

void shell_close_all(void) {
    for (int fd = 0;
         fd < SHELL_FD_MAX;
         fd++) {
        if (shell_files[fd]) {
            vfs_close(shell_files[fd]);
            shell_files[fd] = 0;
        }
    }
}

int shell_handle_command(
    const char* command
) {
    const char* args;

    if (!command) {
        return 0;
    }

    if (command_args(
            command,
            "pwd",
            &args
        ) &&
        *skip_spaces(args) == '\0') {
        print_string(
            shell_cwd,
            0x0F
        );
        print_char('\n', 0x07);
        return 1;
    }

    if (command_args(
            command,
            "ls",
            &args
        )) {
        command_ls(args);
        return 1;
    }

    if (command_args(
            command,
            "cd",
            &args
        )) {
        char path[VFS_PATH_MAX];
        char canonical[VFS_PATH_MAX];
        struct vfs_node* node;

        if (!make_path(
                args,
                path
            )) {
            print_error(
                "cd: ",
                "usage: cd <path>"
            );
            return 1;
        }

        node = vfs_lookup(path);

        if (!node ||
            !vfs_node_is_directory(node) ||
            !vfs_get_path(
                node,
                canonical,
                sizeof(canonical)
            )) {
            print_error(
                "cd: ",
                "directory not found."
            );
            return 1;
        }

        shell_copy(
            shell_cwd,
            canonical,
            sizeof(shell_cwd)
        );
        shell_update_prompt();
        return 1;
    }

    if (command_args(
            command,
            "mkdir",
            &args
        )) {
        char path[VFS_PATH_MAX];

        if (!make_path(args, path) ||
            !vfs_mkdir(path)) {
            print_error(
                "mkdir: ",
                "creation failed."
            );
        }

        return 1;
    }

    if (command_args(
            command,
            "touch",
            &args
        )) {
        char path[VFS_PATH_MAX];

        if (!make_path(args, path) ||
            !vfs_create_file(path)) {
            print_error(
                "touch: ",
                "creation failed."
            );
        }

        return 1;
    }

    if (command_args(
            command,
            "write",
            &args
        )) {
        command_write(args);
        return 1;
    }

    if (command_args(
            command,
            "cat",
            &args
        )) {
        command_cat(args);
        return 1;
    }

    if (command_args(
            command,
            "open",
            &args
        )) {
        command_open(args);
        return 1;
    }

    if (command_args(
            command,
            "read",
            &args
        )) {
        command_read(args);
        return 1;
    }

    if (command_args(
            command,
            "close",
            &args
        )) {
        command_close(args);
        return 1;
    }

    if (command_args(
            command,
            "rm",
            &args
        )) {
        char path[VFS_PATH_MAX];

        if (!make_path(args, path) ||
            !vfs_remove(path)) {
            print_error(
                "rm: ",
                "remove failed; directory must be empty and file closed."
            );
        }

        return 1;
    }

    if (command_args(
            command,
            "history",
            &args
        ) &&
        *skip_spaces(args) == '\0') {
        terminal_print_history();
        return 1;
    }

    if (command_args(
            command,
            "ver",
            &args
        ) &&
        *skip_spaces(args) == '\0') {
        print_string(
            "Michael OS " MICHAEL_OS_VERSION_STRING " - 32-bit x86 experimental OS.",
            0x0E
        );
        print_char('\n', 0x07);
        return 1;
    }

    if (command_args(
            command,
            "dir",
            &args
        )) {
        command_ls(args);
        return 1;
    }

    if (command_args(
            command,
            "type",
            &args
        )) {
        command_cat(args);
        return 1;
    }

    if (command_args(
            command,
            "diskinfo",
            &args
        ) &&
        *skip_spaces(args) == '\0') {
        command_diskinfo();
        return 1;
    }

    if (command_args(
            command,
            "run",
            &args
        )) {
        command_run(args);
        return 1;
    }

    if (command_args(
            command,
            "install-args-test",
            &args
        ) &&
        *skip_spaces(args) == '\0') {
        command_install_args_test();
        return 1;
    }

    if (command_args(
            command,
            "install-exec-test",
            &args
        ) &&
        *skip_spaces(args) == '\0') {
        command_install_exec_test();
        return 1;
    }

    if (command_args(
            command,
            "install-demo",
            &args
        ) &&
        *skip_spaces(args) == '\0') {
        command_install_demo();
        return 1;
    }

    if (command_args(
            command,
            "layout",
            &args
        )) {
        command_layout(args);
        return 1;
    }

    if (command_args(
            command,
            "fstest",
            &args
        ) &&
        *skip_spaces(args) == '\0') {
        command_fstest();
        return 1;
    }

    return 0;
}
