#include "paging.h"
#include "memory.h"

extern void print_char(char c, unsigned char color);
extern void print_string(const char* str, unsigned char color);

extern char _text_start;
extern char _text_end;
extern char _rodata_start;
extern char _rodata_end;
extern char _kernel_end;

#define PAGE_ENTRIES 1024U
#define PAGE_DIRECTORY_FLAG 0x00000083U
#define PAGE_TABLE_FLAG     0x00000003U
#define VM_BITMAP_SIZE (KERNEL_VM_PAGES / 8U)

#define USER_PDE_INDEX (USER_VM_BASE >> 22)
#define KERNEL_VM_TEMP_ADDRESS (KERNEL_VM_END - PAGE_SIZE)

static unsigned int vm_find_free_run(unsigned int count);

static unsigned int page_directory[PAGE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));

static unsigned int low_identity_table[PAGE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));

static unsigned int kernel_vm_tables[16][PAGE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));

static unsigned int vm_used[VM_BITMAP_SIZE];

static int paging_ready = 0;

static unsigned int irq_save_paging(void) {
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

static void irq_restore_paging(unsigned int flags) {
    __asm__ __volatile__(
        "pushl %0\n"
        "popfl"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static void invlpg(unsigned int address) {
    __asm__ __volatile__(
        "invlpg (%0)"
        :
        : "r"(address)
        : "memory"
    );
}

static unsigned int read_cr0(void) {
    unsigned int value;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(value));
    return value;
}

static unsigned int read_cr3(void) {
    unsigned int value;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(value));
    return value;
}

static unsigned int read_cr4(void) {
    unsigned int value;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr3(unsigned int value) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(value) : "memory");
}

static unsigned int align_down(unsigned int value) {
    return value & 0xFFFFF000U;
}

static unsigned int bitmap_byte(unsigned int page) {
    return page >> 3;
}

static unsigned char bitmap_mask(unsigned int page) {
    return (unsigned char)(1U << (page & 7U));
}

static int vm_bitmap_is_set(unsigned int page) {
    return (vm_used[bitmap_byte(page)] &
            bitmap_mask(page)) != 0;
}

static void vm_bitmap_set(unsigned int page) {
    vm_used[bitmap_byte(page)] |= bitmap_mask(page);
}

static void vm_bitmap_clear(unsigned int page) {
    vm_used[bitmap_byte(page)] &=
        (unsigned char)~bitmap_mask(page);
}

static unsigned int section_page_flags(unsigned int address) {
    unsigned int flags = PAGE_PRESENT | PAGE_WRITABLE;

    unsigned int text_start =
        (unsigned int)(unsigned long)&_text_start;
    unsigned int text_end =
        (unsigned int)(unsigned long)&_text_end;
    unsigned int rodata_start =
        (unsigned int)(unsigned long)&_rodata_start;
    unsigned int rodata_end =
        (unsigned int)(unsigned long)&_rodata_end;

    if ((address >= text_start && address < text_end) ||
        (address >= rodata_start && address < rodata_end)) {
        flags &= ~PAGE_WRITABLE;
    }

    return flags;
}

static void initialize_low_identity_table(void) {
    low_identity_table[0] = 0;

    for (unsigned int page = 1; page < PAGE_ENTRIES; page++) {
        unsigned int physical = page * PAGE_SIZE;

        low_identity_table[page] =
            physical | section_page_flags(physical);
    }
}

static void initialize_kernel_vm_tables(void) {
    for (unsigned int table = 0; table < 16; table++) {
        for (unsigned int entry = 0;
             entry < PAGE_ENTRIES;
             entry++) {
            kernel_vm_tables[table][entry] = 0;
        }
    }
}

static void enable_paging(void) {
    unsigned int cr4 = read_cr4();
    unsigned int cr0;

    cr4 |= (1U << 4);
    __asm__ __volatile__(
        "mov %0, %%cr4"
        :
        : "r"(cr4)
        : "memory"
    );

    write_cr3((unsigned int)(unsigned long)page_directory);

    cr0 = read_cr0();
    cr0 |= (1U << 16);
    cr0 |= (1U << 31);

    __asm__ __volatile__(
        "mov %0, %%cr0"
        :
        : "r"(cr0)
        : "memory"
    );
}

int paging_init(void) {
    if ((unsigned int)(unsigned long)&_kernel_end >= 0x00400000U) {
        return 0;
    }

    for (unsigned int i = 0; i < PAGE_ENTRIES; i++) {
        page_directory[i] = 0;
    }

    for (unsigned int i = 1; i < PAGE_ENTRIES; i++) {
        page_directory[i] =
            (i << 22) | PAGE_DIRECTORY_FLAG;
    }

    initialize_low_identity_table();

    page_directory[0] =
        ((unsigned int)(unsigned long)low_identity_table) |
        PAGE_TABLE_FLAG;

    initialize_kernel_vm_tables();

    for (unsigned int table = 0; table < 16; table++) {
        unsigned int pde = 768U + table;

        page_directory[pde] =
            ((unsigned int)(unsigned long)kernel_vm_tables[table]) |
            PAGE_TABLE_FLAG;
    }

    for (unsigned int i = 0; i < VM_BITMAP_SIZE; i++) {
        vm_used[i] = 0;
    }

    vm_bitmap_set(KERNEL_VM_PAGES - 1U);

    enable_paging();

    paging_ready = 1;
    return 1;
}

int paging_is_enabled(void) {
    return paging_ready &&
           (read_cr0() & (1U << 31)) != 0;
}

unsigned int paging_get_directory(void) {
    return read_cr3();
}

unsigned int paging_get_kernel_directory(void) {
    return (unsigned int)(unsigned long)page_directory;
}

int paging_switch_directory(unsigned int directory) {
    if (!paging_ready ||
        (directory & (PAGE_SIZE - 1U)) != 0) {
        return 0;
    }

    write_cr3(directory);
    return 1;
}

int paging_map_page(unsigned int virtual_address,
                    unsigned int physical_address,
                    unsigned int flags) {
    unsigned int table_index;
    unsigned int pte_index;

    if (!paging_ready ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0 ||
        (physical_address & (PAGE_SIZE - 1U)) != 0 ||
        virtual_address < KERNEL_VM_BASE ||
        virtual_address >= KERNEL_VM_END) {
        return 0;
    }

    table_index =
        (virtual_address >> 22) - 768U;
    pte_index =
        (virtual_address >> 12) & 0x3FFU;

    if (table_index >= 16U) {
        return 0;
    }

    kernel_vm_tables[table_index][pte_index] =
        (physical_address & 0xFFFFF000U) |
        (flags & 0x00000FFFU) |
        PAGE_PRESENT;

    invlpg(virtual_address);
    return 1;
}

int paging_unmap_page(unsigned int virtual_address) {
    unsigned int table_index;
    unsigned int pte_index;

    if (!paging_ready ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0 ||
        virtual_address < KERNEL_VM_BASE ||
        virtual_address >= KERNEL_VM_END) {
        return 0;
    }

    table_index =
        (virtual_address >> 22) - 768U;
    pte_index =
        (virtual_address >> 12) & 0x3FFU;

    if (table_index >= 16U) {
        return 0;
    }

    kernel_vm_tables[table_index][pte_index] = 0;
    invlpg(virtual_address);

    return 1;
}

static int get_user_table_physical(
    unsigned int directory,
    unsigned int* table_physical
) {
    unsigned int* directory_ptr;

    if (!paging_ready ||
        !table_physical ||
        (directory & (PAGE_SIZE - 1U)) != 0) {
        return 0;
    }

    if (directory == paging_get_kernel_directory()) {
        unsigned int entry = page_directory[USER_PDE_INDEX];

        /*
         * The kernel directory itself intentionally has no user page table.
         * User mappings are created only inside process address spaces.
         */
        if (!(entry & PAGE_PRESENT) ||
            (entry & (1U << 7))) {
            return 0;
        }

        *table_physical = entry & 0xFFFFF000U;
        return 1;
    }

    directory_ptr =
        (unsigned int*)(unsigned long)KERNEL_VM_TEMP_ADDRESS;

    if (!paging_map_page(
            KERNEL_VM_TEMP_ADDRESS,
            directory,
            PAGE_WRITABLE
        )) {
        return 0;
    }

    {
        unsigned int entry =
            directory_ptr[USER_PDE_INDEX];

        paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

        if (!(entry & PAGE_PRESENT) ||
            (entry & (1U << 7))) {
            return 0;
        }

        *table_physical = entry & 0xFFFFF000U;
    }

    return 1;
}

int paging_map_user_page_in_directory(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int physical_address,
    unsigned int flags
) {
    unsigned int table_physical;
    unsigned int* table;

    if (!paging_ready ||
        (directory & (PAGE_SIZE - 1U)) != 0 ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0 ||
        (physical_address & (PAGE_SIZE - 1U)) != 0 ||
        virtual_address < USER_VM_BASE ||
        virtual_address >= USER_VM_END) {
        return 0;
    }

    if (!get_user_table_physical(
            directory,
            &table_physical
        )) {
        return 0;
    }

    table =
        (unsigned int*)(unsigned long)KERNEL_VM_TEMP_ADDRESS;

    if (!paging_map_page(
            KERNEL_VM_TEMP_ADDRESS,
            table_physical,
            PAGE_WRITABLE
        )) {
        return 0;
    }

    {
        unsigned int index =
            (virtual_address - USER_VM_BASE) >> 12;
        unsigned int entry = table[index];

        if (entry & PAGE_PRESENT) {
            paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);
            return 0;
        }

        table[index] =
            (physical_address & 0xFFFFF000U) |
            (flags & 0x00000FFFU) |
            PAGE_PRESENT |
            PAGE_USER;
    }

    paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

    if (directory == read_cr3()) {
        invlpg(virtual_address);
    }

    return 1;
}

int paging_unmap_user_page_in_directory(
    unsigned int directory,
    unsigned int virtual_address
) {
    unsigned int table_physical;
    unsigned int* table;

    if (!paging_ready ||
        (directory & (PAGE_SIZE - 1U)) != 0 ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0 ||
        virtual_address < USER_VM_BASE ||
        virtual_address >= USER_VM_END) {
        return 0;
    }

    if (!get_user_table_physical(
            directory,
            &table_physical
        )) {
        return 0;
    }

    table =
        (unsigned int*)(unsigned long)KERNEL_VM_TEMP_ADDRESS;

    if (!paging_map_page(
            KERNEL_VM_TEMP_ADDRESS,
            table_physical,
            PAGE_WRITABLE
        )) {
        return 0;
    }

    {
        unsigned int index =
            (virtual_address - USER_VM_BASE) >> 12;

        table[index] = 0;
    }

    paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

    if (directory == read_cr3()) {
        invlpg(virtual_address);
    }

    return 1;
}

int paging_set_user_page_flags_in_directory(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int flags
) {
    unsigned int table_physical;
    unsigned int* table;

    if (!paging_ready ||
        (directory & (PAGE_SIZE - 1U)) != 0 ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0 ||
        virtual_address < USER_VM_BASE ||
        virtual_address >= USER_VM_END) {
        return 0;
    }

    if (!get_user_table_physical(
            directory,
            &table_physical
        )) {
        return 0;
    }

    table =
        (unsigned int*)(unsigned long)KERNEL_VM_TEMP_ADDRESS;

    if (!paging_map_page(
            KERNEL_VM_TEMP_ADDRESS,
            table_physical,
            PAGE_WRITABLE
        )) {
        return 0;
    }

    {
        unsigned int index =
            (virtual_address - USER_VM_BASE) >> 12;
        unsigned int entry = table[index];

        if (!(entry & PAGE_PRESENT) ||
            !(entry & PAGE_USER)) {
            paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);
            return 0;
        }

        table[index] =
            (entry & 0xFFFFF000U) |
            (flags & 0x00000FFFU) |
            PAGE_PRESENT |
            PAGE_USER;
    }

    paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

    if (directory == read_cr3()) {
        invlpg(virtual_address);
    }

    return 1;
}

int paging_map_user_page(unsigned int virtual_address,
                         unsigned int physical_address,
                         unsigned int flags) {
    return paging_map_user_page_in_directory(
        paging_get_directory(),
        virtual_address,
        physical_address,
        flags
    );
}

int paging_unmap_user_page(unsigned int virtual_address) {
    return paging_unmap_user_page_in_directory(
        paging_get_directory(),
        virtual_address
    );
}

unsigned int paging_get_physical_in_directory(
    unsigned int directory,
    unsigned int virtual_address
) {
    unsigned int pde_index;
    unsigned int pte_index;
    unsigned int table_physical;
    unsigned int entry;

    if (!paging_ready ||
        (directory & (PAGE_SIZE - 1U)) != 0) {
        return VM_ALLOC_FAIL;
    }

    pde_index = virtual_address >> 22;
    pte_index = (virtual_address >> 12) & 0x3FFU;

    if (directory == paging_get_kernel_directory()) {
        entry = page_directory[pde_index];

        if (!(entry & PAGE_PRESENT)) {
            return VM_ALLOC_FAIL;
        }

        if (entry & (1U << 7)) {
            return (entry & 0xFFC00000U) |
                   (virtual_address & 0x003FFFFFU);
        }

        table_physical = entry & 0xFFFFF000U;
    } else {
        unsigned int* directory_ptr =
            (unsigned int*)(unsigned long)KERNEL_VM_TEMP_ADDRESS;

        if (!paging_map_page(
                KERNEL_VM_TEMP_ADDRESS,
                directory,
                PAGE_WRITABLE
            )) {
            return VM_ALLOC_FAIL;
        }

        entry = directory_ptr[pde_index];
        paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

        if (!(entry & PAGE_PRESENT)) {
            return VM_ALLOC_FAIL;
        }

        if (entry & (1U << 7)) {
            return (entry & 0xFFC00000U) |
                   (virtual_address & 0x003FFFFFU);
        }

        table_physical = entry & 0xFFFFF000U;
    }

    {
        unsigned int* table =
            (unsigned int*)(unsigned long)KERNEL_VM_TEMP_ADDRESS;

        if (!paging_map_page(
                KERNEL_VM_TEMP_ADDRESS,
                table_physical,
                PAGE_WRITABLE
            )) {
            return VM_ALLOC_FAIL;
        }

        entry = table[pte_index];
        paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);
    }

    if (!(entry & PAGE_PRESENT)) {
        return VM_ALLOC_FAIL;
    }

    return (entry & 0xFFFFF000U) |
           (virtual_address & 0x00000FFFU);
}

unsigned int paging_get_physical(unsigned int virtual_address) {
    return paging_get_physical_in_directory(
        paging_get_directory(),
        virtual_address
    );
}

int paging_user_range_valid_in_directory(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int length,
    int write_access
) {
    unsigned int end;

    if (!paging_ready ||
        length == 0) {
        return 0;
    }

    end = virtual_address + length;

    if (end < virtual_address ||
        virtual_address < USER_VM_BASE ||
        end > USER_VM_END) {
        return 0;
    }

    for (unsigned int address =
             align_down(virtual_address);
         address < end;
         address += PAGE_SIZE) {

        unsigned int physical =
            paging_get_physical_in_directory(
                directory,
                address
            );

        if (physical == VM_ALLOC_FAIL) {
            return 0;
        }

        {
            unsigned int pde_index = address >> 22;
            unsigned int pte_index =
                (address >> 12) & 0x3FFU;
            unsigned int table_physical;
            unsigned int* table;
            unsigned int entry;

            if (!get_user_table_physical(
                    directory,
                    &table_physical
                )) {
                return 0;
            }

            table =
                (unsigned int*)(unsigned long)
                    KERNEL_VM_TEMP_ADDRESS;

            if (!paging_map_page(
                    KERNEL_VM_TEMP_ADDRESS,
                    table_physical,
                    PAGE_WRITABLE
                )) {
                return 0;
            }

            entry = table[pte_index];
            paging_unmap_page(
                KERNEL_VM_TEMP_ADDRESS
            );

            (void)pde_index;

            if (!(entry & PAGE_USER) ||
                (write_access &&
                 !(entry & PAGE_WRITABLE))) {
                return 0;
            }
        }
    }

    return 1;
}

int paging_user_range_valid(unsigned int virtual_address,
                            unsigned int length,
                            int write_access) {
    return paging_user_range_valid_in_directory(
        paging_get_directory(),
        virtual_address,
        length,
        write_access
    );
}

unsigned int paging_create_address_space(void) {
    unsigned int directory_physical;
    unsigned int table_physical;
    unsigned int* directory_ptr;
    unsigned int* table_ptr;

    if (!paging_ready) {
        return VM_ALLOC_FAIL;
    }

    directory_physical = phys_alloc_page();
    table_physical = phys_alloc_page();

    if (directory_physical == VM_ALLOC_FAIL ||
        table_physical == VM_ALLOC_FAIL) {
        if (directory_physical != VM_ALLOC_FAIL) {
            phys_free_page(directory_physical);
        }

        if (table_physical != VM_ALLOC_FAIL) {
            phys_free_page(table_physical);
        }

        return VM_ALLOC_FAIL;
    }

    directory_ptr =
        (unsigned int*)(unsigned long)KERNEL_VM_TEMP_ADDRESS;

    if (!paging_map_page(
            KERNEL_VM_TEMP_ADDRESS,
            directory_physical,
            PAGE_WRITABLE
        )) {
        phys_free_page(directory_physical);
        phys_free_page(table_physical);
        return VM_ALLOC_FAIL;
    }

    for (unsigned int i = 0;
         i < PAGE_ENTRIES;
         i++) {
        directory_ptr[i] = page_directory[i];
    }

    directory_ptr[USER_PDE_INDEX] =
        table_physical |
        PAGE_TABLE_FLAG |
        PAGE_USER;

    paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

    table_ptr =
        (unsigned int*)(unsigned long)KERNEL_VM_TEMP_ADDRESS;

    if (!paging_map_page(
            KERNEL_VM_TEMP_ADDRESS,
            table_physical,
            PAGE_WRITABLE
        )) {
        phys_free_page(directory_physical);
        phys_free_page(table_physical);
        return VM_ALLOC_FAIL;
    }

    for (unsigned int i = 0;
         i < PAGE_ENTRIES;
         i++) {
        table_ptr[i] = 0;
    }

    paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

    return directory_physical;
}

int paging_destroy_address_space(unsigned int directory) {
    unsigned int table_physical;
    unsigned int* table;

    if (!paging_ready ||
        directory == 0 ||
        directory == paging_get_kernel_directory()) {
        return 0;
    }

    if (directory == read_cr3()) {
        if (!paging_switch_directory(
                paging_get_kernel_directory()
            )) {
            return 0;
        }
    }

    if (!get_user_table_physical(
            directory,
            &table_physical
        )) {
        return 0;
    }

    table =
        (unsigned int*)(unsigned long)
            KERNEL_VM_TEMP_ADDRESS;

    if (!paging_map_page(
            KERNEL_VM_TEMP_ADDRESS,
            table_physical,
            PAGE_WRITABLE
        )) {
        return 0;
    }

    for (unsigned int i = 0;
         i < PAGE_ENTRIES;
         i++) {
        unsigned int entry = table[i];

        if ((entry & PAGE_PRESENT) &&
            (entry & PAGE_USER)) {
            phys_free_page(
                entry & 0xFFFFF000U
            );
            table[i] = 0;
        }
    }

    paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

    phys_free_page(table_physical);
    phys_free_page(directory);

    return 1;
}

int paging_allocate_user_pages(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int count,
    unsigned int flags
) {
    if (!paging_ready ||
        count == 0 ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0 ||
        virtual_address < USER_VM_BASE ||
        virtual_address >= USER_VM_END) {
        return 0;
    }

    if ((unsigned long long)virtual_address +
        (unsigned long long)count * PAGE_SIZE >
        USER_VM_END) {
        return 0;
    }

    for (unsigned int i = 0;
         i < count;
         i++) {
        unsigned int physical = phys_alloc_page();

        if (physical == VM_ALLOC_FAIL) {
            paging_free_user_pages(
                directory,
                virtual_address,
                i
            );
            return 0;
        }

        if (!paging_map_user_page_in_directory(
                directory,
                virtual_address + i * PAGE_SIZE,
                physical,
                flags
            )) {
            phys_free_page(physical);

            paging_free_user_pages(
                directory,
                virtual_address,
                i
            );
            return 0;
        }

        {
            unsigned int* page =
                (unsigned int*)(unsigned long)
                    KERNEL_VM_TEMP_ADDRESS;

            if (!paging_map_page(
                    KERNEL_VM_TEMP_ADDRESS,
                    physical,
                    PAGE_WRITABLE
                )) {
                paging_free_user_pages(
                    directory,
                    virtual_address,
                    i + 1U
                );
                return 0;
            }

            for (unsigned int word = 0;
                 word < PAGE_SIZE / sizeof(unsigned int);
                 word++) {
                page[word] = 0;
            }

            paging_unmap_page(
                KERNEL_VM_TEMP_ADDRESS
            );
        }
    }

    return 1;
}

int paging_free_user_pages(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int count
) {
    if (!paging_ready ||
        count == 0 ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0 ||
        virtual_address < USER_VM_BASE ||
        (unsigned long long)virtual_address +
            (unsigned long long)count * PAGE_SIZE >
            USER_VM_END) {
        return 0;
    }

    for (unsigned int i = 0;
         i < count;
         i++) {
        unsigned int physical =
            paging_get_physical_in_directory(
                directory,
                virtual_address + i * PAGE_SIZE
            );

        if (physical == VM_ALLOC_FAIL) {
            return 0;
        }
    }

    for (unsigned int i = 0;
         i < count;
         i++) {
        unsigned int virtual_page =
            virtual_address + i * PAGE_SIZE;
        unsigned int physical =
            paging_get_physical_in_directory(
                directory,
                virtual_page
            );

        paging_unmap_user_page_in_directory(
            directory,
            virtual_page
        );

        if (physical != VM_ALLOC_FAIL) {
            phys_free_page(
                physical & 0xFFFFF000U
            );
        }
    }

    return 1;
}

int paging_write_user_memory(
    unsigned int directory,
    unsigned int virtual_address,
    const void* source,
    unsigned int length
) {
    const unsigned char* src =
        (const unsigned char*)source;
    unsigned int remaining = length;

    if (length == 0) {
        return 1;
    }

    if (!paging_user_range_valid_in_directory(
            directory,
            virtual_address,
            length,
            0
        )) {
        return 0;
    }

    while (remaining > 0) {
        unsigned int page_address =
            align_down(virtual_address);
        unsigned int offset =
            virtual_address - page_address;
        unsigned int chunk =
            PAGE_SIZE - offset;

        if (chunk > remaining) {
            chunk = remaining;
        }

        unsigned int physical =
            paging_get_physical_in_directory(
                directory,
                page_address
            );

        if (physical == VM_ALLOC_FAIL) {
            return 0;
        }

        unsigned char* destination =
            (unsigned char*)(unsigned long)
                KERNEL_VM_TEMP_ADDRESS;

        if (!paging_map_page(
                KERNEL_VM_TEMP_ADDRESS,
                physical & 0xFFFFF000U,
                PAGE_WRITABLE
            )) {
            return 0;
        }

        for (unsigned int i = 0; i < chunk; i++) {
            destination[offset + i] = src[i];
        }

        paging_unmap_page(KERNEL_VM_TEMP_ADDRESS);

        src += chunk;
        virtual_address += chunk;
        remaining -= chunk;
    }

    return 1;
}

unsigned int vm_alloc_pages(unsigned int count,
                            unsigned int flags) {
    unsigned int start;
    unsigned int flags_saved;
    unsigned int mapped = 0;

    if (!paging_ready || count == 0) {
        return VM_ALLOC_FAIL;
    }

    start = vm_find_free_run(count);
    if (start == VM_ALLOC_FAIL) {
        return VM_ALLOC_FAIL;
    }

    flags_saved = irq_save_paging();

    for (unsigned int i = 0;
         i < count;
         i++) {
        unsigned int physical = phys_alloc_page();
        unsigned int virtual_address =
            KERNEL_VM_BASE +
            (start + i) * PAGE_SIZE;

        if (physical == VM_ALLOC_FAIL ||
            !paging_map_page(
                virtual_address,
                physical,
                flags
            )) {

            for (unsigned int j = 0;
                 j < mapped;
                 j++) {
                unsigned int v =
                    KERNEL_VM_BASE +
                    (start + j) * PAGE_SIZE;
                unsigned int phys =
                    paging_get_physical(v);

                if (phys != VM_ALLOC_FAIL) {
                    paging_unmap_page(v);
                    phys_free_page(
                        phys & 0xFFFFF000U
                    );
                }
            }

            irq_restore_paging(flags_saved);
            return VM_ALLOC_FAIL;
        }

        volatile unsigned char* page =
            (volatile unsigned char*)virtual_address;

        for (unsigned int byte = 0;
             byte < PAGE_SIZE;
             byte++) {
            page[byte] = 0;
        }

        mapped++;
    }

    for (unsigned int i = 0;
         i < count;
         i++) {
        vm_bitmap_set(start + i);
    }

    irq_restore_paging(flags_saved);
    return KERNEL_VM_BASE +
           start * PAGE_SIZE;
}

int vm_free_pages(unsigned int virtual_address,
                  unsigned int count) {
    unsigned int start;
    unsigned int flags_saved;

    if (!paging_ready ||
        count == 0 ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0 ||
        virtual_address < KERNEL_VM_BASE ||
        virtual_address >= KERNEL_VM_END) {
        return 0;
    }

    start =
        (virtual_address - KERNEL_VM_BASE) /
        PAGE_SIZE;

    if (start + count > KERNEL_VM_PAGES) {
        return 0;
    }

    for (unsigned int i = 0;
         i < count;
         i++) {
        unsigned int page = start + i;

        if (!vm_bitmap_is_set(page) ||
            paging_get_physical(
                KERNEL_VM_BASE +
                page * PAGE_SIZE
            ) == VM_ALLOC_FAIL) {
            return 0;
        }
    }

    flags_saved = irq_save_paging();

    for (unsigned int i = 0;
         i < count;
         i++) {
        unsigned int v =
            KERNEL_VM_BASE +
            (start + i) * PAGE_SIZE;
        unsigned int phys =
            paging_get_physical(v);

        paging_unmap_page(v);

        if (phys != VM_ALLOC_FAIL) {
            phys_free_page(
                phys & 0xFFFFF000U
            );
        }

        vm_bitmap_clear(start + i);
    }

    irq_restore_paging(flags_saved);
    return 1;
}

static unsigned int vm_find_free_run(unsigned int count) {
    if (count == 0 ||
        count > KERNEL_VM_PAGES) {
        return VM_ALLOC_FAIL;
    }

    for (unsigned int start = 0;
         start + count <= KERNEL_VM_PAGES;
         start++) {
        int free_run = 1;

        for (unsigned int offset = 0;
             offset < count;
             offset++) {

            if (vm_bitmap_is_set(start + offset)) {
                free_run = 0;
                start += offset;
                break;
            }
        }

        if (free_run) {
            return start;
        }
    }

    return VM_ALLOC_FAIL;
}

static void paging_print_uint(unsigned int value,
                              unsigned char color) {
    char digits[10];
    int n = 0;

    if (value == 0) {
        print_char('0', color);
        return;
    }

    while (value > 0 && n < 10) {
        digits[n++] =
            (char)('0' + value % 10U);
        value /= 10U;
    }

    while (n > 0) {
        print_char(digits[--n], color);
    }
}

static void paging_print_hex32(unsigned int value,
                               unsigned char color) {
    const char* hex =
        "0123456789ABCDEF";

    print_string("0x", color);

    for (int shift = 28;
         shift >= 0;
         shift -= 4) {
        print_char(
            hex[(value >> shift) & 0xF],
            color
        );
    }
}

void paging_print_info(void) {
    unsigned int used = 0;

    if (!paging_is_enabled()) {
        print_string(
            "Paging is not enabled.\n",
            0x0C
        );
        return;
    }

    for (unsigned int page = 0;
         page < KERNEL_VM_PAGES;
         page++) {
        if (vm_bitmap_is_set(page)) {
            used++;
        }
    }

    print_string("Paging:\n", 0x0A);
    print_string(
        "Enabled:       yes\n",
        0x0E
    );
    print_string(
        "Page size:     4096 bytes\n",
        0x0E
    );

    print_string("CR3:           ", 0x0E);
    paging_print_hex32(
        read_cr3(),
        0x0F
    );
    print_char('\n', 0x07);

    print_string("CR4.PSE:       ", 0x0E);
    print_string(
        (read_cr4() & (1U << 4))
            ? "enabled\n"
            : "disabled\n",
        0x0F
    );

    print_string("CR0.WP:        ", 0x0E);
    print_string(
        (read_cr0() & (1U << 16))
            ? "enabled\n"
            : "disabled\n",
        0x0F
    );

    print_string("User VM:        ", 0x0E);
    paging_print_hex32(USER_VM_BASE, 0x0F);
    print_string(" - ", 0x07);
    paging_print_hex32(
        USER_VM_END - 1U,
        0x0F
    );
    print_char('\n', 0x07);

    print_string("User heap:      ", 0x0E);
    paging_print_hex32(USER_HEAP_BASE, 0x0F);
    print_string(" - ", 0x07);
    paging_print_hex32(
        USER_HEAP_END - 1U,
        0x0F
    );
    print_char('\n', 0x07);

    print_string("Kernel VM:      ", 0x0E);
    paging_print_hex32(
        KERNEL_VM_BASE,
        0x0F
    );
    print_string(" - ", 0x07);
    paging_print_hex32(
        KERNEL_VM_END - 1U,
        0x0F
    );
    print_char('\n', 0x07);

    print_string("VM pages used: ", 0x0E);
    paging_print_uint(used, 0x0F);
    print_string(" / ", 0x07);
    paging_print_uint(
        KERNEL_VM_PAGES,
        0x0F
    );
    print_char('\n', 0x07);
}

void paging_test(void) {
    unsigned int address;
    unsigned int physical0;
    unsigned int physical1;
    volatile unsigned int* values;

    if (!paging_is_enabled()) {
        print_string(
            "vmtest: paging is not enabled.\n",
            0x0C
        );
        return;
    }

    print_string(
        "Running virtual memory test...\n",
        0x0E
    );

    address =
        vm_alloc_pages(2, PAGE_WRITABLE);

    if (address == VM_ALLOC_FAIL) {
        print_string(
            "vmtest: allocation FAILED.\n",
            0x0C
        );
        return;
    }

    physical0 =
        paging_get_physical(address);
    physical1 =
        paging_get_physical(
            address + PAGE_SIZE
        );

    print_string("Virtual:  ", 0x07);
    paging_print_hex32(address, 0x0F);
    print_string(" ", 0x07);
    paging_print_hex32(
        address + PAGE_SIZE,
        0x0F
    );
    print_char('\n', 0x07);

    print_string("Physical: ", 0x07);
    paging_print_hex32(physical0, 0x0F);
    print_string(" ", 0x07);
    paging_print_hex32(physical1, 0x0F);
    print_char('\n', 0x07);

    if (physical0 == VM_ALLOC_FAIL ||
        physical1 == VM_ALLOC_FAIL) {
        print_string(
            "vmtest: translation FAILED.\n",
            0x0C
        );
        vm_free_pages(address, 2);
        return;
    }

    values =
        (volatile unsigned int*)address;

    values[0] = 0x13579BDFU;
    values[1023] = 0x2468ACE0U;
    values[1024] = 0x55AA55AAU;

    if (values[0] != 0x13579BDFU ||
        values[1023] != 0x2468ACE0U ||
        values[1024] != 0x55AA55AAU) {
        print_string(
            "vmtest: read/write FAILED.\n",
            0x0C
        );
        vm_free_pages(address, 2);
        return;
    }

    if (!vm_free_pages(address, 2)) {
        print_string(
            "vmtest: free FAILED.\n",
            0x0C
        );
        return;
    }

    if (paging_get_physical(address) !=
        VM_ALLOC_FAIL) {
        print_string(
            "vmtest: unmap FAILED.\n",
            0x0C
        );
        return;
    }

    print_string(
        "vmtest: PASS\n",
        0x0A
    );
}

void paging_trigger_page_fault(void) {
    volatile unsigned int* invalid =
        (volatile unsigned int*)0x00000000U;

    *invalid = 0xDEADBEEFU;
}
