#include "memory.h"
#include "paging.h"

extern void print_char(char c, unsigned char color);
extern void print_string(const char* str, unsigned char color);

extern char _kernel_start;
extern char _kernel_end;

#define PAGE_SIZE 4096U
#define PHYS_PAGE_COUNT 1048576U
#define BITMAP_SIZE (PHYS_PAGE_COUNT / 8U)
#define MAX_PHYS_ADDRESS 0x100000000ULL
#define PHYS_ALLOC_FAIL 0xFFFFFFFFU
#define FIRST_ALLOCATABLE_PAGE 256U

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_INFO_MEMORY 0x00000001
#define MULTIBOOT_INFO_MMAP 0x00000040

struct multiboot_info {
    unsigned int flags;
    unsigned int mem_lower;
    unsigned int mem_upper;
    unsigned int boot_device;
    unsigned int cmdline;
    unsigned int mods_count;
    unsigned int mods_addr;
    unsigned int syms[4];
    unsigned int mmap_length;
    unsigned int mmap_addr;
} __attribute__((packed));

struct multiboot_mmap_entry {
    unsigned int size;
    unsigned int base_low;
    unsigned int base_high;
    unsigned int length_low;
    unsigned int length_high;
    unsigned int type;
} __attribute__((packed));

static unsigned char phys_used[BITMAP_SIZE];
static unsigned char phys_reserved[BITMAP_SIZE];

static unsigned int phys_usable_pages = 0;
static unsigned int phys_free_pages_count = 0;

static unsigned int memory_magic = 0;
static unsigned int memory_info_addr = 0;
static int memory_ready = 0;

static inline unsigned int bitmap_byte(unsigned int page) {
    return page >> 3;
}

static inline unsigned char bitmap_mask(unsigned int page) {
    return (unsigned char)(1U << (page & 7));
}

static int bitmap_is_set(const unsigned char* bitmap, unsigned int page) {
    return (bitmap[bitmap_byte(page)] & bitmap_mask(page)) != 0;
}

static void bitmap_set(unsigned char* bitmap, unsigned int page) {
    bitmap[bitmap_byte(page)] |= bitmap_mask(page);
}

static void bitmap_clear(unsigned char* bitmap, unsigned int page) {
    bitmap[bitmap_byte(page)] &= (unsigned char)~bitmap_mask(page);
}

static void set_page_used(unsigned int page, int used) {
    int old_used = bitmap_is_set(phys_used, page);

    if (used) {
        if (!old_used) {
            bitmap_set(phys_used, page);
            if (!bitmap_is_set(phys_reserved, page) &&
                phys_free_pages_count > 0) {
                phys_free_pages_count--;
            }
        }
    } else if (old_used && !bitmap_is_set(phys_reserved, page)) {
        bitmap_clear(phys_used, page);
        phys_free_pages_count++;
    }
}

static void make_available_range(unsigned long long base,
                                 unsigned long long length) {
    if (length == 0 || base >= MAX_PHYS_ADDRESS) {
        return;
    }

    unsigned long long end = base + length;
    if (end < base || end > MAX_PHYS_ADDRESS) {
        end = MAX_PHYS_ADDRESS;
    }

    unsigned long long start_page = (base + PAGE_SIZE - 1ULL) >> 12;
    unsigned long long end_page = end >> 12;

    if (start_page >= end_page || start_page >= PHYS_PAGE_COUNT) {
        return;
    }

    if (end_page > PHYS_PAGE_COUNT) {
        end_page = PHYS_PAGE_COUNT;
    }

    for (unsigned int page = (unsigned int)start_page;
         (unsigned long long)page < end_page;
         page++) {
        if (bitmap_is_set(phys_used, page) &&
            !bitmap_is_set(phys_reserved, page)) {
            bitmap_clear(phys_used, page);
            phys_usable_pages++;
            phys_free_pages_count++;
        }
    }
}

static void reserve_range(unsigned long long base,
                          unsigned long long length) {
    if (length == 0 || base >= MAX_PHYS_ADDRESS) {
        return;
    }

    unsigned long long end = base + length;
    if (end < base || end > MAX_PHYS_ADDRESS) {
        end = MAX_PHYS_ADDRESS;
    }

    unsigned long long start_page = base >> 12;
    unsigned long long end_page = (end + PAGE_SIZE - 1ULL) >> 12;

    if (start_page >= PHYS_PAGE_COUNT) {
        return;
    }

    if (end_page > PHYS_PAGE_COUNT) {
        end_page = PHYS_PAGE_COUNT;
    }

    for (unsigned int page = (unsigned int)start_page;
         (unsigned long long)page < end_page;
         page++) {
        if (!bitmap_is_set(phys_reserved, page)) {
            if (!bitmap_is_set(phys_used, page)) {
                if (phys_usable_pages > 0) {
                    phys_usable_pages--;
                }
                if (phys_free_pages_count > 0) {
                    phys_free_pages_count--;
                }
            }

            bitmap_set(phys_reserved, page);
            bitmap_set(phys_used, page);
        }
    }
}

static int mmap_entry_is_valid(unsigned int current, unsigned int end,
                               const struct multiboot_mmap_entry* entry) {
    if (entry->size < 20) {
        return 0;
    }

    unsigned int next = current + entry->size + 4;
    return next >= current && next <= end;
}

int memory_init(unsigned int magic, unsigned int info_addr) {
    for (unsigned int i = 0; i < BITMAP_SIZE; i++) {
        phys_used[i] = 0xFF;
        phys_reserved[i] = 0;
    }

    phys_usable_pages = 0;
    phys_free_pages_count = 0;
    memory_magic = magic;
    memory_info_addr = info_addr;
    memory_ready = 0;

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC || info_addr == 0) {
        return 0;
    }

    struct multiboot_info* info =
        (struct multiboot_info*)info_addr;

    if (!(info->flags & MULTIBOOT_INFO_MMAP) ||
        info->mmap_addr == 0 || info->mmap_length == 0) {
        return 0;
    }

    unsigned int mmap_end = info->mmap_addr + info->mmap_length;
    if (mmap_end < info->mmap_addr) {
        return 0;
    }

    unsigned int current = info->mmap_addr;
    unsigned int entries = 0;

    while (current < mmap_end && entries < 64) {
        struct multiboot_mmap_entry* entry =
            (struct multiboot_mmap_entry*)current;

        if (!mmap_entry_is_valid(current, mmap_end, entry)) {
            return 0;
        }

        if (entry->type == 1) {
            unsigned long long base =
                ((unsigned long long)entry->base_high << 32) |
                entry->base_low;
            unsigned long long length =
                ((unsigned long long)entry->length_high << 32) |
                entry->length_low;

            make_available_range(base, length);
        }

        current += entry->size + 4;
        entries++;
    }

    if (current != mmap_end) {
        return 0;
    }

    // Protect low memory, kernel image, VGA memory and Multiboot structures.
    reserve_range(0, 0x100000ULL);

    reserve_range(
        (unsigned long long)(unsigned int)&_kernel_start,
        (unsigned long long)(unsigned int)&_kernel_end -
        (unsigned long long)(unsigned int)&_kernel_start
    );

    reserve_range(0xB8000ULL, 0x1000ULL);

    reserve_range(
        (unsigned long long)info_addr,
        sizeof(struct multiboot_info)
    );

    reserve_range(
        (unsigned long long)info->mmap_addr,
        info->mmap_length
    );

    if (phys_free_pages_count == 0) {
        return 0;
    }

    memory_ready = 1;
    return 1;
}

unsigned int phys_alloc_pages(unsigned int count) {
    if (!memory_ready || count == 0 || count > PHYS_PAGE_COUNT) {
        return PHYS_ALLOC_FAIL;
    }

    for (unsigned int start = FIRST_ALLOCATABLE_PAGE;
         start + count <= PHYS_PAGE_COUNT;
         start++) {
        int free_run = 1;

        for (unsigned int offset = 0; offset < count; offset++) {
            unsigned int page = start + offset;

            if (bitmap_is_set(phys_used, page) ||
                bitmap_is_set(phys_reserved, page)) {
                free_run = 0;
                start += offset;
                break;
            }
        }

        if (!free_run) {
            continue;
        }

        for (unsigned int offset = 0; offset < count; offset++) {
            set_page_used(start + offset, 1);
        }

        return start * PAGE_SIZE;
    }

    return PHYS_ALLOC_FAIL;
}

unsigned int phys_alloc_page(void) {
    return phys_alloc_pages(1);
}

int phys_free_pages(unsigned int address, unsigned int count) {
    if (!memory_ready || count == 0 ||
        (address & (PAGE_SIZE - 1U)) != 0) {
        return 0;
    }

    unsigned long long end =
        (unsigned long long)address +
        (unsigned long long)count * PAGE_SIZE;

    if (end > MAX_PHYS_ADDRESS) {
        return 0;
    }

    unsigned int start_page = address >> 12;

    if (start_page < FIRST_ALLOCATABLE_PAGE ||
        start_page + count > PHYS_PAGE_COUNT) {
        return 0;
    }

    for (unsigned int offset = 0; offset < count; offset++) {
        if (bitmap_is_set(phys_reserved, start_page + offset)) {
            return 0;
        }
    }

    for (unsigned int offset = 0; offset < count; offset++) {
        set_page_used(start_page + offset, 0);
    }

    return 1;
}

int phys_free_page(unsigned int address) {
    return phys_free_pages(address, 1);
}

// -----------------------------------------------------------------------------
// Kernel heap backed by virtual pages.
// -----------------------------------------------------------------------------

#define HEAP_BLOCK_MAGIC 0x48454150U
#define HEAP_ALIGNMENT 8U
#define HEAP_HEADER_SIZE 24U
#define HEAP_MIN_CHUNK_PAGES 4U

struct heap_block {
    unsigned int magic;
    unsigned int size;
    unsigned int is_free;
    struct heap_block* next;
    struct heap_block* prev;
    unsigned int reserved;
};

static struct heap_block* heap_head = 0;

static unsigned int align_up(unsigned int value, unsigned int alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static unsigned int irq_save(void) {
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

static void irq_restore(unsigned int flags) {
    __asm__ __volatile__(
        "pushl %0\n"
        "popfl"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static int blocks_are_adjacent(struct heap_block* left,
                               struct heap_block* right) {
    unsigned char* end =
        (unsigned char*)left + HEAP_HEADER_SIZE + left->size;
    return end == (unsigned char*)right;
}

static struct heap_block* heap_grow(unsigned int requested_size) {
    unsigned long long total =
        (unsigned long long)requested_size + HEAP_HEADER_SIZE;

    unsigned long long pages =
        (total + PAGE_SIZE - 1ULL) >> 12;

    if (pages < HEAP_MIN_CHUNK_PAGES) {
        pages = HEAP_MIN_CHUNK_PAGES;
    }

    if (pages >= PHYS_PAGE_COUNT) {
        return 0;
    }

    if (pages > KERNEL_VM_PAGES) {
        return 0;
    }

    unsigned int base =
        vm_alloc_pages((unsigned int)pages, PAGE_WRITABLE);

    if (base == VM_ALLOC_FAIL) {
        return 0;
    }

    unsigned int bytes = (unsigned int)(pages * PAGE_SIZE);
    struct heap_block* block = (struct heap_block*)base;

    block->magic = HEAP_BLOCK_MAGIC;
    block->size = bytes - HEAP_HEADER_SIZE;
    block->is_free = 1;
    block->next = 0;
    block->prev = 0;
    block->reserved = 0;

    if (!heap_head) {
        heap_head = block;
        return block;
    }

    struct heap_block* tail = heap_head;
    while (tail->next) {
        tail = tail->next;
    }

    tail->next = block;
    block->prev = tail;

    return block;
}

static struct heap_block* heap_find_free(unsigned int size) {
    struct heap_block* block = heap_head;

    while (block) {
        if (block->magic == HEAP_BLOCK_MAGIC &&
            block->is_free &&
            block->size >= size) {
            return block;
        }
        block = block->next;
    }

    return 0;
}

static void heap_split(struct heap_block* block, unsigned int size) {
    if (block->size < size + HEAP_HEADER_SIZE + HEAP_ALIGNMENT) {
        return;
    }

    unsigned char* new_address =
        (unsigned char*)block + HEAP_HEADER_SIZE + size;

    struct heap_block* next =
        (struct heap_block*)new_address;

    next->magic = HEAP_BLOCK_MAGIC;
    next->size =
        block->size - size - HEAP_HEADER_SIZE;
    next->is_free = 1;
    next->next = block->next;
    next->prev = block;
    next->reserved = 0;

    if (next->next) {
        next->next->prev = next;
    }

    block->next = next;
    block->size = size;
}

void* malloc(unsigned int size) {
    if (!memory_ready || size == 0) {
        return 0;
    }

    if (size > 0xFFFFFF00U) {
        return 0;
    }

    size = align_up(size, HEAP_ALIGNMENT);

    unsigned int flags = irq_save();

    struct heap_block* block = heap_find_free(size);

    if (!block) {
        block = heap_grow(size);
    }

    if (!block) {
        irq_restore(flags);
        return 0;
    }

    heap_split(block, size);
    block->is_free = 0;

    void* result =
        (void*)((unsigned char*)block + HEAP_HEADER_SIZE);

    irq_restore(flags);
    return result;
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }

    unsigned int flags = irq_save();

    struct heap_block* block =
        (struct heap_block*)((unsigned char*)ptr - HEAP_HEADER_SIZE);

    if (block->magic != HEAP_BLOCK_MAGIC || block->is_free) {
        irq_restore(flags);
        return;
    }

    block->is_free = 1;

    if (block->next &&
        block->next->magic == HEAP_BLOCK_MAGIC &&
        block->next->is_free &&
        blocks_are_adjacent(block, block->next)) {
        struct heap_block* next = block->next;

        block->size += HEAP_HEADER_SIZE + next->size;
        block->next = next->next;

        if (block->next) {
            block->next->prev = block;
        }
    }

    if (block->prev &&
        block->prev->magic == HEAP_BLOCK_MAGIC &&
        block->prev->is_free &&
        blocks_are_adjacent(block->prev, block)) {
        struct heap_block* prev = block->prev;

        prev->size += HEAP_HEADER_SIZE + block->size;
        prev->next = block->next;

        if (prev->next) {
            prev->next->prev = prev;
        }
    }

    irq_restore(flags);
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------

static void mem_print_uint(unsigned int value, unsigned char color) {
    char digits[10];
    int n = 0;

    if (value == 0) {
        print_char('0', color);
        return;
    }

    while (value > 0 && n < 10) {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n > 0) {
        print_char(digits[--n], color);
    }
}

static void mem_print_hex32(unsigned int value, unsigned char color) {
    const char* hex = "0123456789ABCDEF";

    print_string("0x", color);

    for (int shift = 28; shift >= 0; shift -= 4) {
        print_char(hex[(value >> shift) & 0xF], color);
    }
}

static void mem_print_hex64(unsigned int high, unsigned int low,
                            unsigned char color) {
    mem_print_hex32(high, color);
    print_char('_', color);
    mem_print_hex32(low, color);
}

static const char* mmap_type_name(unsigned int type) {
    switch (type) {
        case 1: return "available";
        case 2: return "reserved";
        case 3: return "ACPI reclaimable";
        case 4: return "ACPI NVS";
        case 5: return "bad RAM";
        default: return "unknown";
    }
}

void memory_print_stats(void) {
    if (!memory_ready) {
        print_string(
            "Physical memory manager is not initialized.\n",
            0x0C
        );
        return;
    }

    print_string("Physical allocator:\n", 0x0A);

    print_string("Usable pages: ", 0x0E);
    mem_print_uint(phys_usable_pages, 0x0F);
    print_string(" (", 0x07);
    mem_print_uint(phys_usable_pages * 4U, 0x0F);
    print_string(" KB)\n", 0x07);

    print_string("Free pages:   ", 0x0E);
    mem_print_uint(phys_free_pages_count, 0x0F);
    print_string(" (", 0x07);
    mem_print_uint(phys_free_pages_count * 4U, 0x0F);
    print_string(" KB)\n", 0x07);

    print_string("Allocated:    ", 0x0E);
    mem_print_uint(
        phys_usable_pages - phys_free_pages_count,
        0x0F
    );
    print_string(" pages\n", 0x07);
}

void memory_print_info(void) {
    print_string("Multiboot magic: ", 0x0E);
    mem_print_hex32(memory_magic, 0x0F);
    print_char('\n', 0x07);

    if (!memory_ready || memory_info_addr == 0) {
        print_string(
            "ERROR: memory manager is not ready.\n",
            0x0C
        );
        return;
    }

    struct multiboot_info* info =
        (struct multiboot_info*)memory_info_addr;

    print_string("Info flags: ", 0x0E);
    mem_print_hex32(info->flags, 0x0F);
    print_char('\n', 0x07);

    if (info->flags & MULTIBOOT_INFO_MEMORY) {
        print_string("Lower memory: ", 0x0E);
        mem_print_uint(info->mem_lower, 0x0F);
        print_string(" KB\nUpper memory: ", 0x0E);
        mem_print_uint(info->mem_upper, 0x0F);
        print_string(" KB\n", 0x0E);
    }

    if (!(info->flags & MULTIBOOT_INFO_MMAP)) {
        print_string("Memory map is unavailable.\n", 0x0C);
        memory_print_stats();
        return;
    }

    unsigned int current = info->mmap_addr;
    unsigned int end = info->mmap_addr + info->mmap_length;
    int index = 0;

    print_string("Memory map:\n", 0x0A);

    while (current < end && index < 64) {
        struct multiboot_mmap_entry* entry =
            (struct multiboot_mmap_entry*)current;

        if (!mmap_entry_is_valid(current, end, entry)) {
            print_string(
                "ERROR: malformed memory map entry.\n",
                0x0C
            );
            return;
        }

        print_char('#', 0x07);
        mem_print_uint((unsigned int)index, 0x0F);
        print_string(": ", 0x07);
        print_string(mmap_type_name(entry->type), 0x0E);
        print_string(" base=", 0x07);
        mem_print_hex64(
            entry->base_high,
            entry->base_low,
            0x0F
        );
        print_string(" len=", 0x07);
        mem_print_hex64(
            entry->length_high,
            entry->length_low,
            0x0F
        );
        print_char('\n', 0x07);

        current += entry->size + 4;
        index++;
    }

    if (current != end) {
        print_string(
            "Memory map truncated after 64 entries.\n",
            0x0C
        );
    }

    memory_print_stats();
}

void memory_test(void) {
    if (!memory_ready) {
        print_string(
            "memtest: memory manager is not initialized.\n",
            0x0C
        );
        return;
    }

    print_string("Running virtual kmalloc/free test...\n", 0x0E);

    unsigned char* a = (unsigned char*)malloc(32);
    unsigned char* b = (unsigned char*)malloc(1000);
    unsigned char* c = (unsigned char*)malloc(7000);

    if (!a || !b || !c) {
        print_string("memtest: allocation FAILED.\n", 0x0C);
        free(a);
        free(b);
        free(c);
        return;
    }

    for (unsigned int i = 0; i < 32; i++) {
        a[i] = 0xA5;
    }

    for (unsigned int i = 0; i < 1000; i++) {
        b[i] = 0x5A;
    }

    for (unsigned int i = 0; i < 7000; i++) {
        c[i] = 0x3C;
    }

    int valid = 1;

    for (unsigned int i = 0; i < 32; i++) {
        if (a[i] != 0xA5) {
            valid = 0;
            break;
        }
    }

    for (unsigned int i = 0; i < 1000 && valid; i++) {
        if (b[i] != 0x5A) {
            valid = 0;
            break;
        }
    }

    for (unsigned int i = 0; i < 7000 && valid; i++) {
        if (c[i] != 0x3C) {
            valid = 0;
            break;
        }
    }

    print_string("a = ", 0x07);
    mem_print_hex32((unsigned int)a, 0x0F);
    print_string(" b = ", 0x07);
    mem_print_hex32((unsigned int)b, 0x0F);
    print_string(" c = ", 0x07);
    mem_print_hex32((unsigned int)c, 0x0F);
    print_char('\n', 0x07);

    free(b);
    free(a);

    unsigned char* d = (unsigned char*)malloc(512);

    if (!d) {
        valid = 0;
    } else {
        for (unsigned int i = 0; i < 512; i++) {
            d[i] = 0xC3;
        }

        for (unsigned int i = 0; i < 512; i++) {
            if (d[i] != 0xC3) {
                valid = 0;
                break;
            }
        }
    }

    free(c);
    free(d);

    if (valid) {
        print_string("memtest: PASS\n", 0x0A);
    } else {
        print_string("memtest: FAILED\n", 0x0C);
    }

    memory_print_stats();
}
