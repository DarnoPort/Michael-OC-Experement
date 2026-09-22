#ifndef NANOOS_VFS_H
#define NANOOS_VFS_H

#define VFS_NAME_MAX 32
#define VFS_PATH_MAX 128
#define VFS_MAX_NODES 128
#define VFS_MAX_FILE_SIZE 65536U

#define VFS_NODE_FILE 1U
#define VFS_NODE_DIR  2U

#define VFS_O_READ   0x01U
#define VFS_O_WRITE  0x02U
#define VFS_O_CREATE 0x04U
#define VFS_O_TRUNC  0x08U

struct vfs_node;
struct vfs_file;

int vfs_init(void);

struct vfs_node* vfs_root(void);
struct vfs_node* vfs_lookup(const char* path);
int vfs_node_is_directory(const struct vfs_node* node);
int vfs_node_is_executable(const struct vfs_node* node);
unsigned int vfs_node_size(const struct vfs_node* node);
const char* vfs_node_name(const struct vfs_node* node);
struct vfs_node* vfs_node_parent(const struct vfs_node* node);
struct vfs_node* vfs_node_first_child(const struct vfs_node* node);
struct vfs_node* vfs_node_next_sibling(const struct vfs_node* node);

int vfs_get_path(
    const struct vfs_node* node,
    char* buffer,
    unsigned int buffer_size
);

int vfs_mkdir(const char* path);
int vfs_set_executable(
    const char* path,
    int executable
);
int vfs_create_file(const char* path);
int vfs_remove(const char* path);
int vfs_rename(const char* old_path, const char* new_path);
int vfs_move(const char* old_path, const char* new_path);

struct vfs_file* vfs_open(const char* path, unsigned int flags);
int vfs_close(struct vfs_file* file);
int vfs_read(
    struct vfs_file* file,
    void* buffer,
    unsigned int length
);
int vfs_write(
    struct vfs_file* file,
    const void* buffer,
    unsigned int length
);
int vfs_seek(
    struct vfs_file* file,
    unsigned int offset
);

unsigned int vfs_file_offset(const struct vfs_file* file);

int vfs_truncate(struct vfs_file* file);

#endif
