#ifndef NANOOS_MEMORY_H
#define NANOOS_MEMORY_H

int memory_init(unsigned int magic, unsigned int info_addr);

unsigned int phys_alloc_page(void);
unsigned int phys_alloc_pages(unsigned int count);
int phys_free_page(unsigned int address);
int phys_free_pages(unsigned int address, unsigned int count);

void* malloc(unsigned int size);
void free(void* ptr);

void memory_print_info(void);
void memory_print_stats(void);
void memory_test(void);

#endif
