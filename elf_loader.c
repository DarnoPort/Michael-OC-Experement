#include "elf.h"
#include "paging.h"

extern const unsigned char user_image_start;
extern const unsigned char user_image_end;

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ELFCLASS32 1U
#define ELFDATA2LSB 1U
#define ET_EXEC 2U
#define EM_386 3U
#define PT_LOAD 1U

#define PF_X 1U
#define PF_W 2U
#define PF_R 4U

struct elf32_header {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int e_version;
    unsigned int e_entry;
    unsigned int e_phoff;
    unsigned int e_shoff;
    unsigned int e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
} __attribute__((packed));

struct elf32_program_header {
    unsigned int p_type;
    unsigned int p_offset;
    unsigned int p_vaddr;
    unsigned int p_paddr;
    unsigned int p_filesz;
    unsigned int p_memsz;
    unsigned int p_flags;
    unsigned int p_align;
} __attribute__((packed));

static unsigned int align_up_value(unsigned int value) {
    return (value + PAGE_SIZE - 1U) & 0xFFFFF000U;
}

static int image_range_valid(
    unsigned int image_size,
    unsigned int offset,
    unsigned int length
) {
    return offset <= image_size &&
           length <= image_size - offset;
}

static int user_virtual_range_valid(
    unsigned int address,
    unsigned int length
) {
    unsigned long long end =
        (unsigned long long)address +
        (unsigned long long)length;

    return address >= USER_VM_BASE &&
           address < USER_HEAP_BASE &&
           end > address &&
           end <= USER_HEAP_BASE;
}

static int segment_overlaps_loaded_area(
    unsigned int directory,
    unsigned int start,
    unsigned int end
) {
    unsigned int page = start;

    while (page < end) {
        if (paging_get_physical_in_directory(
                directory,
                page
            ) != VM_ALLOC_FAIL) {
            return 1;
        }

        page += PAGE_SIZE;
    }

    return 0;
}

static int load_segment(
    struct process* process,
    const struct elf32_program_header* ph,
    unsigned int image_size
) {
    unsigned int segment_start;
    unsigned int segment_end;
    unsigned int page_start;
    unsigned int page_end;
    unsigned int page_count;
    unsigned int flags;

    if (ph->p_memsz < ph->p_filesz ||
        !image_range_valid(
            image_size,
            ph->p_offset,
            ph->p_filesz
        )) {
        return 0;
    }

    if (ph->p_memsz == 0) {
        return 1;
    }

    if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr) {
        return 0;
    }

    segment_start = ph->p_vaddr;
    segment_end = ph->p_vaddr + ph->p_memsz;

    if (!user_virtual_range_valid(
            segment_start,
            ph->p_memsz
        )) {
        return 0;
    }

    page_start =
        segment_start & 0xFFFFF000U;
    page_end =
        align_up_value(segment_end);

    if (page_end <= page_start) {
        return 0;
    }

    page_count =
        (page_end - page_start) / PAGE_SIZE;

    if (segment_overlaps_loaded_area(
            process->cr3,
            page_start,
            page_end
        )) {
        return 0;
    }

    /*
     * Load into writable user pages first. After all bytes are copied,
     * tighten the permissions to the ELF PF_* flags.
     */
    flags = PAGE_USER | PAGE_WRITABLE;

    if (!paging_allocate_user_pages(
            process->cr3,
            page_start,
            page_count,
            flags
        )) {
        return 0;
    }

    if (ph->p_filesz > 0) {
        const unsigned char* source =
            user_image_start + ph->p_offset;

        if (!paging_write_user_memory(
                process->cr3,
                segment_start,
                source,
                ph->p_filesz
            )) {
            paging_free_user_pages(
                process->cr3,
                page_start,
                page_count
            );
            return 0;
        }
    }

    /*
     * p_memsz may be larger than p_filesz. The rest is the BSS portion.
     */
    {
        static const unsigned char zeros[256] = {0};
        unsigned int zero_address =
            segment_start + ph->p_filesz;
        unsigned int zero_length =
            ph->p_memsz - ph->p_filesz;

        while (zero_length > 0) {
            unsigned int chunk =
                zero_length < sizeof(zeros)
                    ? zero_length
                    : sizeof(zeros);

            if (!paging_write_user_memory(
                    process->cr3,
                    zero_address,
                    zeros,
                    chunk
                )) {
                paging_free_user_pages(
                    process->cr3,
                    page_start,
                    page_count
                );
                return 0;
            }

            zero_address += chunk;
            zero_length -= chunk;
        }
    }

    {
        unsigned int final_flags =
            PAGE_USER;

        if (ph->p_flags & PF_W) {
            final_flags |= PAGE_WRITABLE;
        }

        for (unsigned int address = page_start;
             address < page_end;
             address += PAGE_SIZE) {

            unsigned int physical =
                paging_get_physical_in_directory(
                    process->cr3,
                    address
                );

            if (physical == VM_ALLOC_FAIL ||
                !paging_set_user_page_flags_in_directory(
                    process->cr3,
                    address,
                    final_flags
                )) {

                paging_free_user_pages(
                    process->cr3,
                    page_start,
                    page_count
                );
                return 0;
            }
        }
    }

    return 1;
}

int elf_load_user_process(struct process* process) {
    const unsigned char* image = user_image_start;
    unsigned int image_size =
        (unsigned int)(
            user_image_end - user_image_start
        );

    const struct elf32_header* header;
    unsigned int ph_end;
    int loaded = 0;

    if (!process || image_size < sizeof(struct elf32_header)) {
        return 0;
    }

    header =
        (const struct elf32_header*)image;

    if (header->e_ident[0] != ELF_MAGIC0 ||
        header->e_ident[1] != ELF_MAGIC1 ||
        header->e_ident[2] != ELF_MAGIC2 ||
        header->e_ident[3] != ELF_MAGIC3 ||
        header->e_ident[4] != ELFCLASS32 ||
        header->e_ident[5] != ELFDATA2LSB ||
        header->e_type != ET_EXEC ||
        header->e_machine != EM_386 ||
        header->e_version != 1U ||
        header->e_entry < USER_VM_BASE ||
        header->e_entry >= USER_HEAP_BASE ||
        header->e_ehsize != sizeof(struct elf32_header) ||
        header->e_phentsize != sizeof(struct elf32_program_header) ||
        header->e_phnum == 0) {
        return 0;
    }

    if (header->e_phoff > image_size) {
        return 0;
    }

    ph_end =
        header->e_phoff +
        (unsigned int)header->e_phnum *
        sizeof(struct elf32_program_header);

    if (ph_end < header->e_phoff ||
        ph_end > image_size) {
        return 0;
    }

    for (unsigned int i = 0;
         i < header->e_phnum;
         i++) {

        const struct elf32_program_header* ph =
            (const struct elf32_program_header*)
                (image +
                 header->e_phoff +
                 i * sizeof(struct elf32_program_header));

        if (ph->p_type != PT_LOAD) {
            continue;
        }

        if (!load_segment(
                process,
                ph,
                image_size
            )) {
            return 0;
        }

        loaded = 1;
    }

    if (!loaded) {
        return 0;
    }

    process->entry_point = header->e_entry;
    return 1;
}
