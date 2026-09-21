; Michael OS Phase 18: embed user_args.elf in the kernel.
[BITS 32]

section .rodata
global user_args_image_start
user_args_image_start:
    incbin "build/user_args.elf"
global user_args_image_end
user_args_image_end:
