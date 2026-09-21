; Embedded Phase 12 user ELF image.
[BITS 32]

section .rodata
align 4

global user_image_start
global user_image_end

user_image_start:
    incbin "build/user_program.elf"
user_image_end:
