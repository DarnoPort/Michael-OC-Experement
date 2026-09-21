#ifndef NANOOS_DISKFS_H
#define NANOOS_DISKFS_H

#define DISKFS_SECTOR_SIZE 512U
#define DISKFS_TOTAL_SECTORS 32768U
#define DISKFS_MAX_INODES 128U
#define DISKFS_NAME_MAX 32U
#define DISKFS_MAX_FILE_SIZE 65536U

#define DISKFS_NODE_FILE 1U
#define DISKFS_NODE_DIR  2U

struct diskfs_inode_info {
    unsigned int used;
    unsigned int type;
    unsigned int size;
    unsigned int data_start;
    unsigned int data_sectors;
    unsigned int parent;
    char name[DISKFS_NAME_MAX];
};

int diskfs_init(void);
int diskfs_was_formatted(void);
int diskfs_get_inode(
    unsigned int inode,
    struct diskfs_inode_info* info
);
int diskfs_create_node(
    unsigned int parent,
    unsigned int type,
    const char* name,
    unsigned int* inode_out
);
int diskfs_remove_node(unsigned int inode);
int diskfs_rename_node(unsigned int inode, const char* new_name);
int diskfs_read_file(
    unsigned int inode,
    void* buffer,
    unsigned int length
);
int diskfs_store_file(
    unsigned int inode,
    const void* buffer,
    unsigned int length
);
int diskfs_truncate_file(unsigned int inode);

int diskfs_get_stats(
    unsigned int* total_sectors,
    unsigned int* free_sectors,
    unsigned int* used_inodes
);

#endif
