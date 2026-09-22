; Michael OS Phase 26.1: embed the standard streams test ELF.
[BITS 32]

section .rodata
align 4

global user_stdio_image_start
global user_stdio_image_end

user_stdio_image_start:
    incbin "build/user_stdio.elf"
user_stdio_image_end:
