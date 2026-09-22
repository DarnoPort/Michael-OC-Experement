#include "vfs.h"
#include "memory.h"
#include "diskfs.h"

struct vfs_node {
    char name[VFS_NAME_MAX];
    unsigned int type;
    unsigned int size;
    unsigned int capacity;
    unsigned int inode;
    unsigned int flags;
    unsigned char* data;

    struct vfs_node* parent;
    struct vfs_node* first_child;
    struct vfs_node* next_sibling;

    unsigned int open_count;
};

struct vfs_file {
    struct vfs_node* node;
    unsigned int offset;
    unsigned int flags;
};

static struct vfs_node root_node;
static unsigned int node_count = 0;
static int vfs_ready = 0;

static unsigned int irq_save_vfs(void) {
    unsigned int flags;

    __asm__ __volatile__(
        "pushfl\n"
        "popl %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void irq_restore_vfs(
    unsigned int flags
) {
    __asm__ __volatile__(
        "pushl %0\n"
        "popfl"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static unsigned int string_length(
    const char* value
) {
    unsigned int length = 0;

    if (!value) {
        return 0;
    }

    while (value[length] != '\0') {
        length++;

        if (length >= VFS_PATH_MAX) {
            break;
        }
    }

    return length;
}

static void copy_name(
    char* destination,
    const char* source,
    unsigned int length
) {
    for (unsigned int i = 0;
         i < length;
         i++) {
        destination[i] = source[i];
    }

    destination[length] = '\0';
}

static int component_is_dot(
    const char* component,
    unsigned int length
) {
    return length == 1U &&
           component[0] == '.';
}

static int component_is_dotdot(
    const char* component,
    unsigned int length
) {
    return length == 2U &&
           component[0] == '.' &&
           component[1] == '.';
}

static struct vfs_node* find_child(
    struct vfs_node* directory,
    const char* name,
    unsigned int length
) {
    struct vfs_node* child =
        directory->first_child;

    while (child) {
        unsigned int child_length =
            string_length(child->name);

        if (child_length == length) {
            int equal = 1;

            for (unsigned int i = 0;
                 i < length;
                 i++) {
                if (child->name[i] != name[i]) {
                    equal = 0;
                    break;
                }
            }

            if (equal) {
                return child;
            }
        }

        child = child->next_sibling;
    }

    return 0;
}

static int parse_component(
    const char* path,
    unsigned int* position,
    char* component,
    unsigned int* length
) {
    unsigned int start;
    unsigned int end;
    unsigned int component_length;

    while (path[*position] == '/') {
        (*position)++;
    }

    if (path[*position] == '\0') {
        *length = 0;
        return 1;
    }

    start = *position;

    while (path[*position] != '\0' &&
           path[*position] != '/') {
        (*position)++;
    }

    end = *position;
    component_length = end - start;

    if (component_length == 0 ||
        component_length >= VFS_NAME_MAX) {
        return 0;
    }

    copy_name(
        component,
        path + start,
        component_length
    );

    *length = component_length;
    return 1;
}

struct vfs_node* vfs_lookup(
    const char* path
) {
    struct vfs_node* current;
    char component[VFS_NAME_MAX];
    unsigned int position = 0;
    unsigned int length;

    if (!vfs_ready ||
        !path ||
        path[0] != '/') {
        return 0;
    }

    current = &root_node;

    for (;;) {
        if (!parse_component(
                path,
                &position,
                component,
                &length
            )) {
            return 0;
        }

        if (length == 0) {
            return current;
        }

        if (component_is_dot(
                component,
                length
            )) {
            continue;
        }

        if (component_is_dotdot(
                component,
                length
            )) {
            if (current->parent) {
                current = current->parent;
            }
            continue;
        }

        if (current->type != VFS_NODE_DIR) {
            return 0;
        }

        current = find_child(
            current,
            component,
            length
        );

        if (!current) {
            return 0;
        }
    }
}

static int resolve_parent(
    const char* path,
    struct vfs_node** parent,
    char* name
) {
    unsigned int path_length;
    unsigned int slash = 0;
    unsigned int name_length;
    char parent_path[VFS_PATH_MAX];

    if (!path ||
        path[0] != '/' ||
        !parent ||
        !name) {
        return 0;
    }

    path_length =
        string_length(path);

    if (path_length == 0 ||
        path_length >= VFS_PATH_MAX ||
        path[path_length - 1U] == '/') {
        return 0;
    }

    for (unsigned int i = 0;
         i < path_length;
         i++) {
        if (path[i] == '/') {
            slash = i;
        }
    }

    name_length =
        path_length - slash - 1U;

    if (name_length == 0 ||
        name_length >= VFS_NAME_MAX) {
        return 0;
    }

    if (slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        for (unsigned int i = 0;
             i < slash;
             i++) {
            parent_path[i] = path[i];
        }

        parent_path[slash] = '\0';
    }

    *parent =
        vfs_lookup(parent_path);

    if (!*parent ||
        (*parent)->type != VFS_NODE_DIR) {
        return 0;
    }

    copy_name(
        name,
        path + slash + 1U,
        name_length
    );

    if (component_is_dot(
            name,
            name_length
        ) ||
        component_is_dotdot(
            name,
            name_length
        )) {
        return 0;
    }

    return 1;
}

static struct vfs_node* allocate_node(
    unsigned int inode,
    unsigned int type,
    const char* name,
    unsigned int size,
    struct vfs_node* parent
) {
    struct vfs_node* node;
    unsigned int name_length;

    if (!name ||
        node_count >= VFS_MAX_NODES ||
        (type != VFS_NODE_FILE &&
         type != VFS_NODE_DIR)) {
        return 0;
    }

    name_length =
        string_length(name);

    if (name_length == 0 ||
        name_length >= VFS_NAME_MAX) {
        return 0;
    }

    node =
        (struct vfs_node*)malloc(
            sizeof(struct vfs_node)
        );

    if (!node) {
        return 0;
    }

    for (unsigned int i = 0;
         i < sizeof(struct vfs_node);
         i++) {
        ((unsigned char*)node)[i] = 0;
    }

    copy_name(
        node->name,
        name,
        name_length
    );

    node->type = type;
    node->size = size;
    node->capacity = 0;
    node->inode = inode;
    node->flags = 0;
    node->parent = parent;

    node_count++;
    return node;
}

static void destroy_node_only(
    struct vfs_node* node
) {
    if (!node) {
        return;
    }

    if (node->data) {
        free(node->data);
    }

    free(node);

    if (node_count > 0) {
        node_count--;
    }
}

static void unlink_node(
    struct vfs_node* node
) {
    struct vfs_node* parent;
    struct vfs_node* current;
    struct vfs_node* previous = 0;

    if (!node ||
        !node->parent) {
        return;
    }

    parent = node->parent;
    current = parent->first_child;

    while (current) {
        if (current == node) {
            if (previous) {
                previous->next_sibling =
                    current->next_sibling;
            } else {
                parent->first_child =
                    current->next_sibling;
            }
            return;
        }

        previous = current;
        current = current->next_sibling;
    }
}

static int load_disk_tree(void) {
    struct vfs_node* map[VFS_MAX_NODES];
    struct diskfs_inode_info info;

    for (unsigned int i = 0;
         i < VFS_MAX_NODES;
         i++) {
        map[i] = 0;
    }

    map[0] = &root_node;

    for (unsigned int inode = 1;
         inode < VFS_MAX_NODES;
         inode++) {
        struct vfs_node* node;

        if (!diskfs_get_inode(
                inode,
                &info
            )) {
            return 0;
        }

        if (!info.used) {
            continue;
        }

        if ((info.type != VFS_NODE_FILE &&
             info.type != VFS_NODE_DIR) ||
            info.parent >= VFS_MAX_NODES ||
            info.size > VFS_MAX_FILE_SIZE ||
            (info.flags & ~DISKFS_FLAG_SUPPORTED) != 0 ||
            (info.type == VFS_NODE_DIR && info.flags != 0U)) {
            return 0;
        }

        if (info.name[0] == '\0') {
            return 0;
        }

        node =
            node =
            allocate_node(
                inode,
                info.type,
                info.name,
                info.size,
                0
            );

        if (!node) {
            return 0;
        }

        node->flags = info.flags;
        map[inode] = node;
    }

    for (unsigned int inode = 1;
         inode < VFS_MAX_NODES;
         inode++) {
        if (!map[inode]) {
            continue;
        }

        if (!diskfs_get_inode(
                inode,
                &info
            ) ||
            !map[info.parent] ||
            !vfs_node_is_directory(
                map[info.parent]
            ) ||
            info.parent == inode) {
            return 0;
        }

        map[inode]->parent =
            map[info.parent];

        map[inode]->next_sibling =
            map[info.parent]->first_child;

        map[info.parent]->first_child =
            map[inode];
    }

    return 1;
}

int vfs_init(void) {
    unsigned int flags;
    struct diskfs_check_report report;

    flags = irq_save_vfs();

    for (unsigned int i = 0;
         i < sizeof(root_node);
         i++) {
        ((unsigned char*)&root_node)[i] = 0;
    }

    node_count = 1;
    vfs_ready = 0;

    if (!diskfs_init()) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (!diskfs_check(&report) ||
        report.errors != 0U) {
        irq_restore_vfs(flags);
        return 0;
    }

    root_node.name[0] = '/';
    root_node.name[1] = '\0';
    root_node.type = VFS_NODE_DIR;
    root_node.inode = 0;

    vfs_ready = 1;

    if (!load_disk_tree()) {
        vfs_ready = 0;
        irq_restore_vfs(flags);
        return 0;
    }

    irq_restore_vfs(flags);
    return 1;
}

struct vfs_node* vfs_root(void) {
    return vfs_ready
        ? &root_node
        : 0;
}

int vfs_node_is_directory(
    const struct vfs_node* node
) {
    return node &&
           node->type == VFS_NODE_DIR;
}

int vfs_node_is_executable(
    const struct vfs_node* node
) {
    return node &&
           node->type == VFS_NODE_FILE &&
           (node->flags & DISKFS_FLAG_EXECUTABLE) != 0;
}

unsigned int vfs_node_size(
    const struct vfs_node* node
) {
    return node ? node->size : 0;
}

const char* vfs_node_name(
    const struct vfs_node* node
) {
    return node
        ? node->name
        : "";
}

struct vfs_node* vfs_node_parent(
    const struct vfs_node* node
) {
    return node ? node->parent : 0;
}

struct vfs_node* vfs_node_first_child(
    const struct vfs_node* node
) {
    return node
        ? node->first_child
        : 0;
}

struct vfs_node* vfs_node_next_sibling(
    const struct vfs_node* node
) {
    return node
        ? node->next_sibling
        : 0;
}

int vfs_get_path(
    const struct vfs_node* node,
    char* buffer,
    unsigned int buffer_size
) {
    const struct vfs_node* current;
    const struct vfs_node* stack[VFS_MAX_NODES];
    unsigned int depth = 0;
    unsigned int position = 0;

    if (!vfs_ready ||
        !node ||
        !buffer ||
        buffer_size < 2U) {
        return 0;
    }

    current = node;

    while (current &&
           current != &root_node) {
        if (depth >= VFS_MAX_NODES) {
            return 0;
        }

        stack[depth++] = current;
        current = current->parent;
    }

    if (!current) {
        return 0;
    }

    buffer[position++] = '/';

    for (unsigned int i = depth;
         i > 0;
         i--) {
        const char* name =
            stack[i - 1U]->name;
        unsigned int length =
            string_length(name);

        if (position + length +
            (i > 1U ? 1U : 0U) >=
            buffer_size) {
            return 0;
        }

        for (unsigned int j = 0;
             j < length;
             j++) {
            buffer[position++] =
                name[j];
        }

        if (i > 1U) {
            buffer[position++] = '/';
        }
    }

    if (position >= buffer_size) {
        return 0;
    }

    buffer[position] = '\0';
    return 1;
}

static struct vfs_node* create_node(
    const char* path,
    unsigned int type
) {
    struct vfs_node* parent;
    struct vfs_node* node;
    char name[VFS_NAME_MAX];
    unsigned int inode;
    unsigned int flags;

    if (node_count >= VFS_MAX_NODES ||
        (type != VFS_NODE_FILE &&
         type != VFS_NODE_DIR)) {
        return 0;
    }

    flags = irq_save_vfs();

    if (!resolve_parent(
            path,
            &parent,
            name
        )) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (find_child(
            parent,
            name,
            string_length(name)
        )) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (!diskfs_create_node(
            parent->inode,
            type == VFS_NODE_FILE
                ? DISKFS_NODE_FILE
                : DISKFS_NODE_DIR,
            name,
            &inode
        )) {
        irq_restore_vfs(flags);
        return 0;
    }

    node =
        allocate_node(
            inode,
            type,
            name,
            0,
            parent
        );

    if (!node) {
        (void)diskfs_remove_node(inode);
        irq_restore_vfs(flags);
        return 0;
    }

    node->next_sibling =
        parent->first_child;

    parent->first_child = node;

    irq_restore_vfs(flags);
    return node;
}

int vfs_mkdir(
    const char* path
) {
    return create_node(
        path,
        VFS_NODE_DIR
    ) != 0;
}

int vfs_set_executable(
    const char* path,
    int executable
) {
    struct vfs_node* node;
    unsigned int flags;
    unsigned int irq_flags;

    if (!vfs_ready ||
        !path ||
        path[0] != '/') {
        return 0;
    }

    irq_flags = irq_save_vfs();

    node = vfs_lookup(path);

    if (!node ||
        node->type != VFS_NODE_FILE) {
        irq_restore_vfs(irq_flags);
        return 0;
    }

    flags = executable
        ? DISKFS_FLAG_EXECUTABLE
        : 0U;

    if (!diskfs_set_flags(
            node->inode,
            flags
        )) {
        irq_restore_vfs(irq_flags);
        return 0;
    }

    node->flags = flags;

    irq_restore_vfs(irq_flags);
    return 1;
}

int vfs_create_file(
    const char* path
) {
    return create_node(
        path,
        VFS_NODE_FILE
    ) != 0;
}

int vfs_remove(
    const char* path
) {
    struct vfs_node* node;
    unsigned int flags;

    if (!vfs_ready ||
        !path ||
        path[0] != '/') {
        return 0;
    }

    flags = irq_save_vfs();

    node = vfs_lookup(path);

    if (!node ||
        node == &root_node ||
        node->open_count != 0 ||
        (node->type == VFS_NODE_DIR &&
         node->first_child != 0)) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (!diskfs_remove_node(
            node->inode
        )) {
        irq_restore_vfs(flags);
        return 0;
    }

    unlink_node(node);
    destroy_node_only(node);

    irq_restore_vfs(flags);
    return 1;
}


int vfs_rename(
    const char* old_path,
    const char* new_path
) {
    struct vfs_node* node;
    struct vfs_node* old_parent;
    struct vfs_node* new_parent;
    struct vfs_node* existing;
    char new_name[VFS_NAME_MAX];
    unsigned int flags;

    if (!vfs_ready ||
        !old_path ||
        !new_path ||
        old_path[0] != '/' ||
        new_path[0] != '/') {
        return 0;
    }

    flags = irq_save_vfs();

    node = vfs_lookup(old_path);

    if (!node ||
        node == &root_node ||
        node->open_count != 0) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (!resolve_parent(
            old_path,
            &old_parent,
            new_name
        )) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (node->parent != old_parent) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (!resolve_parent(
            new_path,
            &new_parent,
            new_name
        ) ||
        new_parent != old_parent) {
        irq_restore_vfs(flags);
        return 0;
    }

    existing = vfs_lookup(new_path);

    if (existing) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (!diskfs_rename_node(
            node->inode,
            new_name
        )) {
        irq_restore_vfs(flags);
        return 0;
    }

    copy_name(
        node->name,
        new_name,
        string_length(new_name)
    );

    irq_restore_vfs(flags);
    return 1;
}


int vfs_move(
    const char* old_path,
    const char* new_path
) {
    struct vfs_node* node;
    struct vfs_node* old_parent;
    struct vfs_node* new_parent;
    struct vfs_node* current;
    struct vfs_node* existing;
    char old_name[VFS_NAME_MAX];
    char new_name[VFS_NAME_MAX];
    unsigned int flags;

    if (!vfs_ready ||
        !old_path ||
        !new_path ||
        old_path[0] != '/' ||
        new_path[0] != '/') {
        return 0;
    }

    flags = irq_save_vfs();

    node = vfs_lookup(old_path);

    if (!node ||
        node == &root_node ||
        node->open_count != 0 ||
        !resolve_parent(
            old_path,
            &old_parent,
            old_name
        ) ||
        node->parent != old_parent ||
        !resolve_parent(
            new_path,
            &new_parent,
            new_name
        ) ||
        !new_parent ||
        !vfs_node_is_directory(new_parent)) {
        irq_restore_vfs(flags);
        return 0;
    }

    existing = vfs_lookup(new_path);

    if (existing) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (node->type == VFS_NODE_DIR) {
        current = new_parent;

        while (current) {
            if (current == node) {
                irq_restore_vfs(flags);
                return 0;
            }

            current = current->parent;
        }
    }

    if (!diskfs_move_node(
            node->inode,
            new_parent->inode,
            new_name
        )) {
        irq_restore_vfs(flags);
        return 0;
    }

    unlink_node(node);

    node->parent = new_parent;
    node->next_sibling =
        new_parent->first_child;
    new_parent->first_child = node;

    irq_restore_vfs(flags);
    return 1;
}


static int grow_file(
    struct vfs_node* node,
    unsigned int required
) {
    unsigned int new_capacity;
    unsigned char* new_data;

    if (required > VFS_MAX_FILE_SIZE) {
        return 0;
    }

    if (required <= node->capacity) {
        return 1;
    }

    new_capacity = node->capacity;

    if (new_capacity == 0) {
        new_capacity = 256U;
    }

    while (new_capacity < required) {
        if (new_capacity >
            VFS_MAX_FILE_SIZE / 2U) {
            new_capacity =
                VFS_MAX_FILE_SIZE;
            break;
        }

        new_capacity *= 2U;
    }

    new_data =
        (unsigned char*)malloc(
            new_capacity
        );

    if (!new_data) {
        return 0;
    }

    for (unsigned int i = 0;
         i < new_capacity;
         i++) {
        new_data[i] = 0;
    }

    for (unsigned int i = 0;
         i < node->size;
         i++) {
        new_data[i] =
            node->data[i];
    }

    if (node->data) {
        free(node->data);
    }

    node->data = new_data;
    node->capacity = new_capacity;
    return 1;
}

static int ensure_file_data(
    struct vfs_node* node
) {
    if (!node ||
        node->type != VFS_NODE_FILE) {
        return 0;
    }

    if (node->size == 0) {
        return 1;
    }

    if (node->data) {
        return 1;
    }

    node->capacity =
        node->size > 256U
            ? node->size
            : 256U;

    node->data =
        (unsigned char*)malloc(
            node->capacity
        );

    if (!node->data) {
        node->capacity = 0;
        return 0;
    }

    if (!diskfs_read_file(
            node->inode,
            node->data,
            node->size
        )) {
        free(node->data);
        node->data = 0;
        node->capacity = 0;
        return 0;
    }

    return 1;
}

struct vfs_file* vfs_open(
    const char* path,
    unsigned int flags
) {
    struct vfs_node* node;
    struct vfs_file* file;
    unsigned int irq_flags;
    int created = 0;

    if (!vfs_ready ||
        !path ||
        path[0] != '/' ||
        !(flags & (
            VFS_O_READ |
            VFS_O_WRITE
        ))) {
        return 0;
    }

    irq_flags = irq_save_vfs();

    node = vfs_lookup(path);

    if (!node &&
        (flags & VFS_O_CREATE)) {
        node =
            create_node(
                path,
                VFS_NODE_FILE
            );

        if (node) {
            created = 1;
        }
    }

    if (!node ||
        node->type != VFS_NODE_FILE) {
        irq_restore_vfs(irq_flags);
        return 0;
    }

    if ((flags & VFS_O_TRUNC) &&
        (flags & VFS_O_WRITE)) {
        if (!diskfs_truncate_file(
                node->inode
            )) {
            if (created) {
                (void)diskfs_remove_node(
                    node->inode
                );
                unlink_node(node);
                destroy_node_only(node);
            }

            irq_restore_vfs(irq_flags);
            return 0;
        }

        if (node->data) {
            free(node->data);
            node->data = 0;
        }

        node->size = 0;
        node->capacity = 0;
    } else if (node->size > 0 &&
               !ensure_file_data(node)) {
        if (created) {
            (void)diskfs_remove_node(
                node->inode
            );
            unlink_node(node);
            destroy_node_only(node);
        }

        irq_restore_vfs(irq_flags);
        return 0;
    }

    file =
        (struct vfs_file*)malloc(
            sizeof(struct vfs_file)
        );

    if (!file) {
        if (created) {
            (void)diskfs_remove_node(
                node->inode
            );
            unlink_node(node);
            destroy_node_only(node);
        }

        irq_restore_vfs(irq_flags);
        return 0;
    }

    file->node = node;
    file->offset = 0;
    file->flags =
        flags & (
            VFS_O_READ |
            VFS_O_WRITE
        );

    node->open_count++;

    irq_restore_vfs(irq_flags);
    return file;
}

int vfs_close(
    struct vfs_file* file
) {
    unsigned int flags;

    if (!file ||
        !file->node) {
        return 0;
    }

    flags = irq_save_vfs();

    if (file->node->open_count > 0) {
        file->node->open_count--;
    }

    if (file->node->open_count == 0 &&
        file->node->data) {
        free(file->node->data);
        file->node->data = 0;
        file->node->capacity = 0;
    }

    free(file);

    irq_restore_vfs(flags);
    return 1;
}

int vfs_read(
    struct vfs_file* file,
    void* buffer,
    unsigned int length
) {
    unsigned int available;
    unsigned int flags;

    if (!file ||
        !file->node ||
        file->node->type != VFS_NODE_FILE ||
        !(file->flags & VFS_O_READ) ||
        !buffer) {
        return -1;
    }

    if (length == 0) {
        return 0;
    }

    flags = irq_save_vfs();

    if (file->offset >=
        file->node->size) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (!ensure_file_data(
            file->node
        )) {
        irq_restore_vfs(flags);
        return -1;
    }

    available =
        file->node->size -
        file->offset;

    if (length > available) {
        length = available;
    }

    for (unsigned int i = 0;
         i < length;
         i++) {
        ((unsigned char*)buffer)[i] =
            file->node->data[
                file->offset + i
            ];
    }

    file->offset += length;

    irq_restore_vfs(flags);
    return (int)length;
}

int vfs_write(
    struct vfs_file* file,
    const void* buffer,
    unsigned int length
) {
    unsigned int required;
    unsigned int old_size;
    unsigned int old_offset;
    unsigned int flags;

    if (!file ||
        !file->node ||
        file->node->type != VFS_NODE_FILE ||
        !(file->flags & VFS_O_WRITE) ||
        !buffer) {
        return -1;
    }

    if (length == 0) {
        return 0;
    }

    if (file->offset >
        VFS_MAX_FILE_SIZE ||
        length >
        VFS_MAX_FILE_SIZE -
            file->offset) {
        return -1;
    }

    flags = irq_save_vfs();

    if (!ensure_file_data(
            file->node
        ) ||
        !grow_file(
            file->node,
            file->offset + length
        )) {
        irq_restore_vfs(flags);
        return -1;
    }

    old_size = file->node->size;
    old_offset = file->offset;
    required =
        file->offset + length;

    for (unsigned int i = 0;
         i < length;
         i++) {
        file->node->data[
            file->offset + i
        ] =
            ((const unsigned char*)buffer)[i];
    }

    if (required > file->node->size) {
        file->node->size = required;
    }

    if (!diskfs_store_file(
            file->node->inode,
            file->node->data,
            file->node->size
        )) {
        file->node->size = old_size;
        file->offset = old_offset;

        if (old_size == 0) {
            if (file->node->data) {
                free(file->node->data);
            }

            file->node->data = 0;
            file->node->capacity = 0;
        } else {
            (void)diskfs_read_file(
                file->node->inode,
                file->node->data,
                old_size
            );
        }

        irq_restore_vfs(flags);
        return -1;
    }

    file->offset =
        old_offset + length;

    irq_restore_vfs(flags);
    return (int)length;
}

int vfs_seek(
    struct vfs_file* file,
    unsigned int offset
) {
    if (!file ||
        !file->node ||
        file->node->type != VFS_NODE_FILE ||
        offset > VFS_MAX_FILE_SIZE) {
        return 0;
    }

    file->offset = offset;
    return 1;
}

unsigned int vfs_file_offset(
    const struct vfs_file* file
) {
    return file
        ? file->offset
        : 0;
}

int vfs_truncate(
    struct vfs_file* file
) {
    unsigned int flags;

    if (!file ||
        !file->node ||
        file->node->type != VFS_NODE_FILE ||
        !(file->flags & VFS_O_WRITE)) {
        return 0;
    }

    flags = irq_save_vfs();

    if (!diskfs_truncate_file(
            file->node->inode
        )) {
        irq_restore_vfs(flags);
        return 0;
    }

    if (file->node->data) {
        free(file->node->data);
        file->node->data = 0;
    }

    file->node->size = 0;
    file->node->capacity = 0;
    file->offset = 0;

    irq_restore_vfs(flags);
    return 1;
}
