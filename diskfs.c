#include "diskfs.h"
#include "ata.h"

#define DISKFS_MAGIC0 'N'
#define DISKFS_MAGIC1 'F'
#define DISKFS_MAGIC2 'S'
#define DISKFS_MAGIC3 '1'
#define DISKFS_VERSION 1U

#define DISKFS_SUPERBLOCK_SECTOR 0U
#define DISKFS_INODE_START       1U
#define DISKFS_INODE_SECTORS     16U
#define DISKFS_BITMAP_START      17U
#define DISKFS_BITMAP_SECTORS    8U
#define DISKFS_DATA_START        25U

#define DISKFS_BITMAP_BYTES     (DISKFS_BITMAP_SECTORS * DISKFS_SECTOR_SIZE)

struct diskfs_superblock {
    unsigned char magic[4];
    unsigned int version;
    unsigned int sector_size;
    unsigned int total_sectors;
    unsigned int inode_start;
    unsigned int inode_count;
    unsigned int bitmap_start;
    unsigned int bitmap_sectors;
    unsigned int data_start;
    unsigned int reserved[6];
} __attribute__((packed));

struct diskfs_inode {
    unsigned int used;
    unsigned int type;
    unsigned int size;
    unsigned int data_start;
    unsigned int data_sectors;
    unsigned int parent;
    char name[DISKFS_NAME_MAX];
    unsigned int reserved[2];
} __attribute__((packed));

static int diskfs_ready = 0;
static int diskfs_formatted = 0;

static unsigned char bitmap[DISKFS_BITMAP_BYTES];
static unsigned char sector_buffer[DISKFS_SECTOR_SIZE];
static unsigned char inode_buffer[DISKFS_SECTOR_SIZE];
static unsigned char copy_buffer[DISKFS_SECTOR_SIZE];
static unsigned char check_bitmap[DISKFS_BITMAP_BYTES];

static unsigned int min_u(
    unsigned int a,
    unsigned int b
) {
    return a < b ? a : b;
}

static void zero_memory(
    void* address,
    unsigned int length
) {
    unsigned char* bytes =
        (unsigned char*)address;

    for (unsigned int i = 0;
         i < length;
         i++) {
        bytes[i] = 0;
    }
}

static void copy_memory(
    void* destination,
    const void* source,
    unsigned int length
) {
    unsigned char* dst =
        (unsigned char*)destination;
    const unsigned char* src =
        (const unsigned char*)source;

    for (unsigned int i = 0;
         i < length;
         i++) {
        dst[i] = src[i];
    }
}

static unsigned int inode_sector(
    unsigned int inode
) {
    return DISKFS_INODE_START +
           inode / 8U;
}

static unsigned int inode_offset(
    unsigned int inode
) {
    return (inode % 8U) *
           sizeof(struct diskfs_inode);
}

static int bitmap_bit_is_set(
    unsigned int index
) {
    return (
        bitmap[index >> 3] &
        (1U << (index & 7U))
    ) != 0;
}

static void bitmap_set(
    unsigned int index,
    int used
) {
    if (used) {
        bitmap[index >> 3] |=
            (unsigned char)(
                1U << (index & 7U)
            );
    } else {
        bitmap[index >> 3] &=
            (unsigned char)(
                ~(1U << (index & 7U))
            );
    }
}

static int bitmap_load(void) {
    for (unsigned int sector = 0;
         sector < DISKFS_BITMAP_SECTORS;
         sector++) {
        if (!ata_read_sector(
                DISKFS_BITMAP_START + sector,
                bitmap +
                    sector * DISKFS_SECTOR_SIZE
            )) {
            return 0;
        }
    }

    return 1;
}

static int bitmap_save(void) {
    for (unsigned int sector = 0;
         sector < DISKFS_BITMAP_SECTORS;
         sector++) {
        if (!ata_write_sector(
                DISKFS_BITMAP_START + sector,
                bitmap +
                    sector * DISKFS_SECTOR_SIZE
            )) {
            return 0;
        }
    }

    return ata_flush();
}

static int write_superblock(void) {
    struct diskfs_superblock* super;

    zero_memory(
        sector_buffer,
        sizeof(sector_buffer)
    );

    super =
        (struct diskfs_superblock*)sector_buffer;

    super->magic[0] = DISKFS_MAGIC0;
    super->magic[1] = DISKFS_MAGIC1;
    super->magic[2] = DISKFS_MAGIC2;
    super->magic[3] = DISKFS_MAGIC3;
    super->version = DISKFS_VERSION;
    super->sector_size = DISKFS_SECTOR_SIZE;
    super->total_sectors = DISKFS_TOTAL_SECTORS;
    super->inode_start = DISKFS_INODE_START;
    super->inode_count = DISKFS_MAX_INODES;
    super->bitmap_start = DISKFS_BITMAP_START;
    super->bitmap_sectors = DISKFS_BITMAP_SECTORS;
    super->data_start = DISKFS_DATA_START;

    return
        ata_write_sector(
            DISKFS_SUPERBLOCK_SECTOR,
            sector_buffer
        ) &&
        ata_flush();
}

static int write_inode(
    unsigned int inode,
    const struct diskfs_inode* value
) {
    unsigned int sector;
    unsigned int offset;

    if (inode >= DISKFS_MAX_INODES ||
        !value) {
        return 0;
    }

    sector = inode_sector(inode);
    offset = inode_offset(inode);

    if (!ata_read_sector(
            sector,
            inode_buffer
        )) {
        return 0;
    }

    copy_memory(
        inode_buffer + offset,
        value,
        sizeof(*value)
    );

    return
        ata_write_sector(
            sector,
            inode_buffer
        ) &&
        ata_flush();
}

static int read_inode(
    unsigned int inode,
    struct diskfs_inode* value
) {
    if (inode >= DISKFS_MAX_INODES ||
        !value) {
        return 0;
    }

    if (!ata_read_sector(
            inode_sector(inode),
            inode_buffer
        )) {
        return 0;
    }

    copy_memory(
        value,
        inode_buffer + inode_offset(inode),
        sizeof(*value)
    );

    return 1;
}

static int find_free_inode(
    unsigned int* inode_out
) {
    struct diskfs_inode inode;

    if (!inode_out) {
        return 0;
    }

    for (unsigned int i = 1;
         i < DISKFS_MAX_INODES;
         i++) {
        if (!read_inode(i, &inode)) {
            return 0;
        }

        if (!inode.used) {
            *inode_out = i;
            return 1;
        }
    }

    return 0;
}

static unsigned int data_sector_count(void) {
    return DISKFS_TOTAL_SECTORS -
           DISKFS_DATA_START;
}

static int find_free_run(
    unsigned int required,
    unsigned int* start_out
) {
    unsigned int run = 0;
    unsigned int run_start = 0;
    unsigned int available =
        data_sector_count();

    if (!required ||
        required > available ||
        !start_out) {
        return 0;
    }

    for (unsigned int index = 0;
         index < available;
         index++) {
        if (!bitmap_bit_is_set(index)) {
            if (run == 0) {
                run_start = index;
            }

            run++;

            if (run >= required) {
                *start_out =
                    DISKFS_DATA_START +
                    run_start;
                return 1;
            }
        } else {
            run = 0;
        }
    }

    return 0;
}

static void free_run(
    unsigned int start,
    unsigned int count
) {
    if (start < DISKFS_DATA_START ||
        count == 0 ||
        start + count >
            DISKFS_TOTAL_SECTORS) {
        return;
    }

    for (unsigned int sector = 0;
         sector < count;
         sector++) {
        bitmap_set(
            start -
                DISKFS_DATA_START +
                sector,
            0
        );
    }
}

static int mark_run_used(
    unsigned int start,
    unsigned int count
) {
    if (start < DISKFS_DATA_START ||
        count == 0 ||
        start + count >
            DISKFS_TOTAL_SECTORS) {
        return 0;
    }

    for (unsigned int sector = 0;
         sector < count;
         sector++) {
        unsigned int index =
            start -
            DISKFS_DATA_START +
            sector;

        if (bitmap_bit_is_set(index)) {
            return 0;
        }
    }

    for (unsigned int sector = 0;
         sector < count;
         sector++) {
        bitmap_set(
            start -
                DISKFS_DATA_START +
                sector,
            1
        );
    }

    return 1;
}

static int create_blank_inode(
    unsigned int inode_number,
    unsigned int parent,
    unsigned int type,
    const char* name
) {
    struct diskfs_inode inode;

    zero_memory(
        &inode,
        sizeof(inode)
    );

    inode.used = 1;
    inode.type = type;
    inode.parent = parent;

    for (unsigned int i = 0;
         i < DISKFS_NAME_MAX - 1U &&
         name[i] != '\0';
         i++) {
        inode.name[i] = name[i];
    }

    return write_inode(
        inode_number,
        &inode
    );
}

static int diskfs_name_equal(
    const struct diskfs_inode* inode,
    const char* name
) {
    unsigned int i = 0;

    if (!inode || !name) {
        return 0;
    }

    while (i < DISKFS_NAME_MAX &&
           inode->name[i] != '\0' &&
           name[i] != '\0') {
        if (inode->name[i] != name[i]) {
            return 0;
        }

        i++;
    }

    return i < DISKFS_NAME_MAX &&
           inode->name[i] == '\0' &&
           name[i] == '\0';
}

static int validate_node_name(
    const char* name
) {
    unsigned int length = 0;

    if (!name) {
        return 0;
    }

    while (length < DISKFS_NAME_MAX) {
        char value = name[length];

        if (value == '\0') {
            return length > 0U;
        }

        if (value == '/' ||
            length >= DISKFS_NAME_MAX - 1U) {
            return 0;
        }

        length++;
    }

    return 0;
}

int diskfs_init(void) {
    struct diskfs_superblock super;
    struct diskfs_inode root;

    diskfs_ready = 0;
    diskfs_formatted = 0;

    if (!ata_init()) {
        return 0;
    }

    if (!ata_read_sector(
            DISKFS_SUPERBLOCK_SECTOR,
            sector_buffer
        )) {
        return 0;
    }

    copy_memory(
        &super,
        sector_buffer,
        sizeof(super)
    );

    if (super.magic[0] != DISKFS_MAGIC0 ||
        super.magic[1] != DISKFS_MAGIC1 ||
        super.magic[2] != DISKFS_MAGIC2 ||
        super.magic[3] != DISKFS_MAGIC3 ||
        super.version != DISKFS_VERSION ||
        super.sector_size != DISKFS_SECTOR_SIZE ||
        super.total_sectors != DISKFS_TOTAL_SECTORS ||
        super.inode_start != DISKFS_INODE_START ||
        super.inode_count != DISKFS_MAX_INODES ||
        super.bitmap_start != DISKFS_BITMAP_START ||
        super.bitmap_sectors != DISKFS_BITMAP_SECTORS ||
        super.data_start != DISKFS_DATA_START) {

        if (!write_superblock()) {
            return 0;
        }

        zero_memory(
            bitmap,
            sizeof(bitmap)
        );

        if (!bitmap_save()) {
            return 0;
        }

        zero_memory(
            &root,
            sizeof(root)
        );

        root.used = 1;
        root.type = DISKFS_NODE_DIR;
        root.parent = 0;
        root.name[0] = '/';

        if (!write_inode(0, &root)) {
            return 0;
        }

        for (unsigned int i = 1;
             i < DISKFS_MAX_INODES;
             i++) {
            zero_memory(
                &root,
                sizeof(root)
            );

            if (!write_inode(i, &root)) {
                return 0;
            }
        }

        diskfs_formatted = 1;
    } else {
        if (!bitmap_load() ||
            !read_inode(0, &root) ||
            !root.used ||
            root.type != DISKFS_NODE_DIR ||
            root.parent != 0 ||
            root.name[0] != '/') {
            return 0;
        }
    }

    diskfs_ready = 1;
    return 1;
}

int diskfs_was_formatted(void) {
    return diskfs_formatted;
}

int diskfs_get_inode(
    unsigned int inode,
    struct diskfs_inode_info* info
) {
    struct diskfs_inode value;

    if (!diskfs_ready ||
        !info ||
        !read_inode(inode, &value)) {
        return 0;
    }

    info->used = value.used;
    info->type = value.type;
    info->size = value.size;
    info->data_start = value.data_start;
    info->data_sectors = value.data_sectors;
    info->parent = value.parent;

    for (unsigned int i = 0;
         i < DISKFS_NAME_MAX;
         i++) {
        info->name[i] = value.name[i];
    }

    return 1;
}

int diskfs_create_node(
    unsigned int parent,
    unsigned int type,
    const char* name,
    unsigned int* inode_out
) {
    struct diskfs_inode candidate;
    unsigned int inode;

    if (!diskfs_ready ||
        parent >= DISKFS_MAX_INODES ||
        (type != DISKFS_NODE_FILE &&
         type != DISKFS_NODE_DIR) ||
        !validate_node_name(name) ||
        !inode_out) {
        return 0;
    }

    if (!read_inode(
            parent,
            &candidate
        ) ||
        !candidate.used ||
        candidate.type != DISKFS_NODE_DIR) {
        return 0;
    }

    for (unsigned int i = 1;
         i < DISKFS_MAX_INODES;
         i++) {
        if (!read_inode(i, &candidate)) {
            return 0;
        }

        if (candidate.used &&
            candidate.parent == parent &&
            diskfs_name_equal(
                &candidate,
                name
            )) {
            return 0;
        }
    }

    if (!find_free_inode(&inode)) {
        return 0;
    }

    if (!create_blank_inode(
            inode,
            parent,
            type,
            name
        )) {
        return 0;
    }

    *inode_out = inode;
    return 1;
}


int diskfs_rename_node(
    unsigned int inode_number,
    const char* new_name
) {
    struct diskfs_inode inode;
    struct diskfs_inode candidate;

    if (!diskfs_ready ||
        inode_number == 0 ||
        inode_number >= DISKFS_MAX_INODES ||
        !validate_node_name(new_name) ||
        !read_inode(
            inode_number,
            &inode
        ) ||
        !inode.used) {
        return 0;
    }

    for (unsigned int i = 1;
         i < DISKFS_MAX_INODES;
         i++) {
        if (i == inode_number) {
            continue;
        }

        if (!read_inode(i, &candidate)) {
            return 0;
        }

        if (candidate.used &&
            candidate.parent == inode.parent &&
            diskfs_name_equal(
                &candidate,
                new_name
            )) {
            return 0;
        }
    }

    zero_memory(
        inode.name,
        sizeof(inode.name)
    );

    for (unsigned int i = 0;
         i < DISKFS_NAME_MAX - 1U &&
         new_name[i] != '\0';
         i++) {
        inode.name[i] = new_name[i];
    }

    return write_inode(
        inode_number,
        &inode
    );
}


int diskfs_move_node(
    unsigned int inode_number,
    unsigned int new_parent,
    const char* new_name
) {
    struct diskfs_inode inode;
    struct diskfs_inode candidate;
    
    if (!diskfs_ready ||
        inode_number == 0 ||
        inode_number >= DISKFS_MAX_INODES ||
        new_parent >= DISKFS_MAX_INODES ||
        inode_number == new_parent ||
        !validate_node_name(new_name) ||
        !read_inode(
            inode_number,
            &inode
        ) ||
        !inode.used ||
        !read_inode(
            new_parent,
            &candidate
        ) ||
        !candidate.used ||
        candidate.type != DISKFS_NODE_DIR) {
        return 0;
    }

    for (unsigned int i = 1;
         i < DISKFS_MAX_INODES;
         i++) {
        if (i == inode_number) {
            continue;
        }

        if (!read_inode(i, &candidate)) {
            return 0;
        }

        if (candidate.used &&
            candidate.parent == new_parent &&
            diskfs_name_equal(
                &candidate,
                new_name
            )) {
            return 0;
        }
    }

    inode.parent = new_parent;

    zero_memory(
        inode.name,
        sizeof(inode.name)
    );

    for (unsigned int i = 0;
         i < DISKFS_NAME_MAX - 1U &&
         new_name[i] != '\0';
         i++) {
        inode.name[i] = new_name[i];
    }

    return write_inode(
        inode_number,
        &inode
    );
}

int diskfs_remove_node(
    unsigned int inode_number
) {
    struct diskfs_inode inode;
    struct diskfs_inode candidate;
    unsigned int old_start;
    unsigned int old_sectors;

    if (!diskfs_ready ||
        inode_number == 0 ||
        inode_number >= DISKFS_MAX_INODES ||
        !read_inode(
            inode_number,
            &inode
        ) ||
        !inode.used) {
        return 0;
    }

    if (inode.type == DISKFS_NODE_DIR) {
        for (unsigned int i = 1;
             i < DISKFS_MAX_INODES;
             i++) {
            if (!read_inode(i, &candidate)) {
                return 0;
            }

            if (candidate.used &&
                candidate.parent == inode_number) {
                return 0;
            }
        }
    }

    old_start = inode.data_start;
    old_sectors = inode.data_sectors;

    zero_memory(
        &inode,
        sizeof(inode)
    );

    /*
     * Invalidate the inode first. If updating the bitmap subsequently
     * fails, the only consequence is leaked old data sectors, not a
     * live inode pointing at sectors that may be reused.
     */
    if (!write_inode(
            inode_number,
            &inode
        )) {
        return 0;
    }

    if (old_sectors) {
        free_run(
            old_start,
            old_sectors
        );

        if (!bitmap_save()) {
            return 1;
        }
    }

    return 1;
}

int diskfs_read_file(
    unsigned int inode_number,
    void* buffer,
    unsigned int length
) {
    struct diskfs_inode inode;
    unsigned int sectors;

    if (!diskfs_ready ||
        inode_number >= DISKFS_MAX_INODES ||
        !buffer ||
        !read_inode(
            inode_number,
            &inode
        ) ||
        !inode.used ||
        inode.type != DISKFS_NODE_FILE ||
        length > inode.size) {
        return 0;
    }

    if (length == 0) {
        return 1;
    }

    if (inode.data_start < DISKFS_DATA_START ||
        inode.data_sectors == 0 ||
        inode.data_start + inode.data_sectors >
            DISKFS_TOTAL_SECTORS) {
        return 0;
    }

    sectors =
        (length + DISKFS_SECTOR_SIZE - 1U) /
        DISKFS_SECTOR_SIZE;

    if (sectors > inode.data_sectors) {
        return 0;
    }

    for (unsigned int i = 0;
         i < sectors;
         i++) {
        unsigned int offset =
            i * DISKFS_SECTOR_SIZE;
        unsigned int chunk =
            min_u(
                DISKFS_SECTOR_SIZE,
                length - offset
            );

        if (!ata_read_sector(
                inode.data_start + i,
                copy_buffer
            )) {
            return 0;
        }

        copy_memory(
            (unsigned char*)buffer + offset,
            copy_buffer,
            chunk
        );
    }

    return 1;
}

int diskfs_store_file(
    unsigned int inode_number,
    const void* buffer,
    unsigned int length
) {
    struct diskfs_inode inode;
    struct diskfs_inode updated;
    unsigned int old_start;
    unsigned int old_sectors;
    unsigned int required_sectors = 0;
    unsigned int new_start = 0;

    if (!diskfs_ready ||
        inode_number >= DISKFS_MAX_INODES ||
        length > DISKFS_MAX_FILE_SIZE ||
        (length > 0U && !buffer) ||
        !read_inode(
            inode_number,
            &inode
        ) ||
        !inode.used ||
        inode.type != DISKFS_NODE_FILE) {
        return 0;
    }

    if (length > 0) {
        required_sectors =
            (length +
             DISKFS_SECTOR_SIZE - 1U) /
            DISKFS_SECTOR_SIZE;

        if (!find_free_run(
                required_sectors,
                &new_start
            ) ||
            !mark_run_used(
                new_start,
                required_sectors
            )) {
            return 0;
        }

        if (!bitmap_save()) {
            free_run(
                new_start,
                required_sectors
            );
            (void)bitmap_save();
            return 0;
        }

        for (unsigned int i = 0;
             i < required_sectors;
             i++) {
            unsigned int offset =
                i * DISKFS_SECTOR_SIZE;
            unsigned int remaining =
                length > offset
                    ? length - offset
                    : 0;
            unsigned int chunk =
                min_u(
                    DISKFS_SECTOR_SIZE,
                    remaining
                );

            zero_memory(
                sector_buffer,
                sizeof(sector_buffer)
            );

            if (chunk > 0) {
                copy_memory(
                    sector_buffer,
                    (const unsigned char*)buffer +
                        offset,
                    chunk
                );
            }

            if (!ata_write_sector(
                    new_start + i,
                    sector_buffer
                )) {
                free_run(
                    new_start,
                    required_sectors
                );
                (void)bitmap_save();
                return 0;
            }
        }

        if (!ata_flush()) {
            free_run(
                new_start,
                required_sectors
            );
            (void)bitmap_save();
            return 0;
        }
    }

    old_start = inode.data_start;
    old_sectors = inode.data_sectors;

    updated = inode;
    updated.size = length;
    updated.data_start = new_start;
    updated.data_sectors =
        required_sectors;

    if (!write_inode(
            inode_number,
            &updated
        )) {
        if (required_sectors) {
            free_run(
                new_start,
                required_sectors
            );
            (void)bitmap_save();
        }
        return 0;
    }

    if (old_sectors) {
        free_run(
            old_start,
            old_sectors
        );

        /*
         * The inode already points at the new allocation. A failed bitmap
         * write can leak old sectors, but it cannot corrupt the new file.
         */
        (void)bitmap_save();
    }

    return 1;
}

int diskfs_truncate_file(
    unsigned int inode_number
) {
    struct diskfs_inode inode;
    struct diskfs_inode updated;
    unsigned int old_start;
    unsigned int old_sectors;

    if (!diskfs_ready ||
        inode_number >= DISKFS_MAX_INODES ||
        !read_inode(
            inode_number,
            &inode
        ) ||
        !inode.used ||
        inode.type != DISKFS_NODE_FILE) {
        return 0;
    }

    old_start = inode.data_start;
    old_sectors = inode.data_sectors;

    updated = inode;
    updated.size = 0;
    updated.data_start = 0;
    updated.data_sectors = 0;

    if (!write_inode(
            inode_number,
            &updated
        )) {
        return 0;
    }

    if (old_sectors) {
        free_run(
            old_start,
            old_sectors
        );
        (void)bitmap_save();
    }

    return 1;
}


static void check_bitmap_set(
    unsigned int index
) {
    check_bitmap[index >> 3] |=
        (unsigned char)(
            1U << (index & 7U)
        );
}

static int check_inode_parent_chain(
    unsigned int inode_number
) {
    struct diskfs_inode inode;
    unsigned int current = inode_number;

    for (unsigned int depth = 0;
         depth < DISKFS_MAX_INODES;
         depth++) {
        if (current == 0U) {
            return 1;
        }

        if (!read_inode(current, &inode) ||
            !inode.used ||
            inode.parent >= DISKFS_MAX_INODES) {
            return 0;
        }

        current = inode.parent;
    }

    return 0;
}

int diskfs_check(
    struct diskfs_check_report* report
) {
    unsigned int actual_allocated = 0;
    unsigned int referenced = 0;
    unsigned int used_inodes = 0;
    unsigned int errors = 0;

    if (!diskfs_ready ||
        !report) {
        return 0;
    }

    zero_memory(
        check_bitmap,
        sizeof(check_bitmap)
    );

    for (unsigned int i = 0;
         i < DISKFS_MAX_INODES;
         i++) {
        struct diskfs_inode inode;

        if (!read_inode(i, &inode)) {
            return 0;
        }

        if (!inode.used) {
            continue;
        }

        if (inode.used != 1U) {
            errors++;
        }

        used_inodes++;

        if (i == 0U) {
            if (inode.used != 1U ||
                inode.type != DISKFS_NODE_DIR ||
                inode.parent != 0U ||
                !diskfs_name_equal(
                    &inode,
                    "/"
                )) {
                errors++;
            }
            if (inode.size != 0U ||
                inode.data_start != 0U ||
                inode.data_sectors != 0U) {
                errors++;
            }
            continue;
        }

        if ((inode.type != DISKFS_NODE_FILE &&
             inode.type != DISKFS_NODE_DIR) ||
            inode.parent >= DISKFS_MAX_INODES ||
            inode.parent == i ||
            !validate_node_name(inode.name)) {
            errors++;
            continue;
        }

        {
            struct diskfs_inode parent;

            if (!read_inode(
                    inode.parent,
                    &parent
                ) ||
                !parent.used ||
                parent.type != DISKFS_NODE_DIR) {
                errors++;
            }
        }

        if (!check_inode_parent_chain(i)) {
            errors++;
        }

        for (unsigned int other = i + 1U;
             other < DISKFS_MAX_INODES;
             other++) {
            struct diskfs_inode candidate;

            if (!read_inode(other, &candidate)) {
                return 0;
            }

            if (candidate.used &&
                candidate.parent == inode.parent &&
                diskfs_name_equal(
                    &candidate,
                    inode.name
                )) {
                errors++;
            }
        }

        if (inode.type == DISKFS_NODE_DIR) {
            if (inode.size != 0U ||
                inode.data_start != 0U ||
                inode.data_sectors != 0U) {
                errors++;
            }
            continue;
        }

        if (inode.size > DISKFS_MAX_FILE_SIZE) {
            errors++;
            continue;
        }

        {
            unsigned int required =
                inode.size == 0U
                    ? 0U
                    : (
                        (inode.size +
                         DISKFS_SECTOR_SIZE - 1U) /
                        DISKFS_SECTOR_SIZE
                    );

            if (inode.data_sectors != required) {
                errors++;
                continue;
            }

            if (required == 0U) {
                if (inode.data_start != 0U) {
                    errors++;
                }
                continue;
            }

            if (inode.data_start < DISKFS_DATA_START ||
                inode.data_start + required >
                    DISKFS_TOTAL_SECTORS) {
                errors++;
                continue;
            }

            for (unsigned int sector = 0;
                 sector < required;
                 sector++) {
                unsigned int absolute =
                    inode.data_start + sector;
                unsigned int index =
                    absolute - DISKFS_DATA_START;

                if (bitmap_bit_is_set(index)) {
                    if (
                        check_bitmap[index >> 3] &
                        (unsigned char)(
                            1U << (index & 7U)
                        )
                    ) {
                        errors++;
                    } else {
                        check_bitmap_set(index);
                        referenced++;
                    }
                } else {
                    errors++;
                    check_bitmap_set(index);
                    referenced++;
                }
            }
        }
    }

    for (unsigned int i = 0;
         i < data_sector_count();
         i++) {
        if (bitmap_bit_is_set(i)) {
            actual_allocated++;
        }

        if (
            (bitmap_bit_is_set(i) ? 1U : 0U) !=
            (
                (check_bitmap[i >> 3] &
                 (unsigned char)(
                     1U << (i & 7U)
                 )) != 0
                    ? 1U
                    : 0U
            )
        ) {
            errors++;
        }
    }

    for (unsigned int i = data_sector_count();
         i < DISKFS_BITMAP_BYTES * 8U;
         i++) {
        if (bitmap_bit_is_set(i)) {
            errors++;
        }
    }

    report->used_inodes = used_inodes;
    report->allocated_sectors = actual_allocated;
    report->referenced_sectors = referenced;
    report->errors = errors;

    return 1;
}

int diskfs_get_stats(
    unsigned int* total_sectors,
    unsigned int* free_sectors,
    unsigned int* used_inodes
) {
    unsigned int free_count = 0;
    unsigned int inode_count = 0;

    if (!diskfs_ready) {
        return 0;
    }

    for (unsigned int i = 0;
         i < data_sector_count();
         i++) {
        if (!bitmap_bit_is_set(i)) {
            free_count++;
        }
    }

    for (unsigned int i = 0;
         i < DISKFS_MAX_INODES;
         i++) {
        struct diskfs_inode inode;

        if (!read_inode(i, &inode)) {
            return 0;
        }

        if (inode.used) {
            inode_count++;
        }
    }

    if (total_sectors) {
        *total_sectors = DISKFS_TOTAL_SECTORS;
    }

    if (free_sectors) {
        *free_sectors = free_count;
    }

    if (used_inodes) {
        *used_inodes = inode_count;
    }

    return 1;
}
