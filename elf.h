#ifndef NANOOS_ELF_H
#define NANOOS_ELF_H

#include "process.h"

int elf_load_user_process_from_image(
    struct process* process,
    const unsigned char* image,
    unsigned int image_size
);
int elf_load_user_process(struct process* process);

#endif
