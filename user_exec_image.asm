; Embedded Phase 17 exec-test ELF image.
[BITS 32]

section .rodata
align 4

global user_exec_image_start
global user_exec_image_end

user_exec_image_start:
    incbin "build/user_exec.elf"
user_exec_image_end:
