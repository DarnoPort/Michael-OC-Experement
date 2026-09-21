#ifndef NANOOS_PAGING_H
#define NANOOS_PAGING_H

#define PAGE_SIZE 4096U

#define PAGE_PRESENT  0x001U
#define PAGE_WRITABLE 0x002U
#define PAGE_USER     0x004U

#define VM_ALLOC_FAIL 0xFFFFFFFFU

#define KERNEL_VM_BASE 0xC0000000U
#define KERNEL_VM_SIZE 0x04000000U
#define KERNEL_VM_END  0xC4000000U
#define KERNEL_VM_PAGES (KERNEL_VM_SIZE / PAGE_SIZE)

#define USER_VM_BASE 0x80000000U
#define USER_VM_SIZE 0x00400000U
#define USER_VM_END  0x80400000U

#define USER_HEAP_BASE 0x80100000U
#define USER_HEAP_END  0x803F0000U

int paging_init(void);
int paging_is_enabled(void);
unsigned int paging_get_directory(void);
unsigned int paging_get_kernel_directory(void);
int paging_switch_directory(unsigned int directory);

int paging_map_page(unsigned int virtual_address,
                    unsigned int physical_address,
                    unsigned int flags);
int paging_unmap_page(unsigned int virtual_address);

int paging_map_user_page(unsigned int virtual_address,
                         unsigned int physical_address,
                         unsigned int flags);
int paging_unmap_user_page(unsigned int virtual_address);

unsigned int paging_get_physical(unsigned int virtual_address);
unsigned int paging_get_physical_in_directory(
    unsigned int directory,
    unsigned int virtual_address
);

int paging_user_range_valid(unsigned int virtual_address,
                            unsigned int length,
                            int write_access);

int paging_user_range_valid_in_directory(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int length,
    int write_access
);

unsigned int paging_create_address_space(void);
int paging_destroy_address_space(unsigned int directory);

int paging_map_user_page_in_directory(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int physical_address,
    unsigned int flags
);

int paging_unmap_user_page_in_directory(
    unsigned int directory,
    unsigned int virtual_address
);

int paging_set_user_page_flags_in_directory(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int flags
);

int paging_allocate_user_pages(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int count,
    unsigned int flags
);

int paging_free_user_pages(
    unsigned int directory,
    unsigned int virtual_address,
    unsigned int count
);

int paging_write_user_memory(
    unsigned int directory,
    unsigned int virtual_address,
    const void* source,
    unsigned int length
);

int paging_read_user_memory(
    unsigned int directory,
    unsigned int virtual_address,
    void* destination,
    unsigned int length
);

unsigned int vm_alloc_pages(unsigned int count, unsigned int flags);
int vm_free_pages(unsigned int virtual_address, unsigned int count);

void paging_print_info(void);
void paging_test(void);
void paging_trigger_page_fault(void);

#endif
