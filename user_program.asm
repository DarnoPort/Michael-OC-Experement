; NanoOS Phase 13 user program.
; Loaded as an ELF32 PT_LOAD image at 0x80000000.
;
; Demonstrates:
; - Ring 3 syscalls
; - per-process PID
; - per-process user heap through sbrk
; - scheduler yield
; - preemptive timer switching

[BITS 32]

SYS_EXIT   equ 0
SYS_WRITE  equ 1
SYS_FD_WRITE equ 11
SYS_GETPID equ 2
SYS_YIELD  equ 3
SYS_SBRK        equ 4
SYS_OPEN        equ 5
SYS_FILE_READ   equ 6
SYS_FILE_WRITE  equ 7
SYS_CLOSE       equ 8

VFS_O_READ   equ 1
VFS_O_WRITE  equ 2
VFS_O_CREATE equ 4

USER_HEAP_BYTES equ 4096

section .text
global user_program_start

user_program_start:
    ; Allocate one private heap page.
    mov eax, SYS_SBRK
    mov ebx, USER_HEAP_BYTES
    int 0x80

    cmp eax, 0xFFFFFFFF
    je .exit

    mov esi, eax
    mov edi, 8

.loop:
    ; Get current PID.
    mov eax, SYS_GETPID
    int 0x80

    ; Format a one-digit demo PID followed by newline.
    ; The demo creates low PIDs, so one byte is sufficient here.
    add al, '0'
    mov byte [esi], al
    mov byte [esi + 1], 10

    ; Build a per-process path: /worker1.txt or /worker2.txt.
    mov byte [esi + 16], '/'
    mov byte [esi + 17], 'w'
    mov byte [esi + 18], 'o'
    mov byte [esi + 19], 'r'
    mov byte [esi + 20], 'k'
    mov byte [esi + 21], 'e'
    mov byte [esi + 22], 'r'
    mov al, [esi]
    mov byte [esi + 23], al
    mov byte [esi + 24], '.'
    mov byte [esi + 25], 't'
    mov byte [esi + 26], 'x'
    mov byte [esi + 27], 't'
    mov byte [esi + 28], 0

    ; Write to the terminal.
    mov eax, SYS_FD_WRITE
    mov ebx, 1
    mov ecx, esi
    mov edx, 2
    int 0x80

    ; Open the process-private RAMFS file.
    mov eax, SYS_OPEN
    lea ebx, [esi + 16]
    mov ecx, VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE
    int 0x80

    cmp eax, 0xFFFFFFFF
    je .exit

    mov ebp, eax

    ; Write the PID bytes through the file syscall.
    mov eax, SYS_FILE_WRITE
    mov ebx, ebp
    mov ecx, esi
    mov edx, 2
    int 0x80

    ; Close the write handle.
    mov eax, SYS_CLOSE
    mov ebx, ebp
    int 0x80

    ; Re-open the same file for reading.
    mov eax, SYS_OPEN
    lea ebx, [esi + 16]
    mov ecx, VFS_O_READ
    int 0x80

    cmp eax, 0xFFFFFFFF
    je .exit

    mov ebp, eax

    ; Read the two bytes back into the private heap.
    mov eax, SYS_FILE_READ
    mov ebx, ebp
    lea ecx, [esi + 2]
    mov edx, 2
    int 0x80

    cmp eax, 2
    jne .close_and_exit

    ; Verify that RAMFS returned exactly what we wrote.
    mov al, [esi + 2]
    cmp al, [esi]
    jne .close_and_exit

    mov al, [esi + 3]
    cmp al, [esi + 1]
    jne .close_and_exit

    mov eax, SYS_CLOSE
    mov ebx, ebp
    int 0x80

    ; Intentional CPU load so the timer can preempt us.
    mov ecx, 5000000
.delay:
    dec ecx
    jnz .delay

    ; Also demonstrate voluntary scheduling.
    mov eax, SYS_YIELD
    int 0x80

    dec edi
    jnz .loop

.close_and_exit:
    mov eax, SYS_CLOSE
    mov ebx, ebp
    int 0x80

.exit:
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang
