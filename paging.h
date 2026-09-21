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

int paging_init(void);
int paging_is_enabled(void);
unsigned int paging_get_directory(void);

int paging_map_page(unsigned int virtual_address,
                    unsigned int physical_address,
                    unsigned int flags);
int paging_unmap_page(unsigned int virtual_address);
unsigned int paging_get_physical(unsigned int virtual_address);

unsigned int vm_alloc_pages(unsigned int count, unsigned int flags);
int vm_free_pages(unsigned int virtual_address, unsigned int count);

void paging_print_info(void);
void paging_test(void);
void paging_trigger_page_fault(void);

#endif
