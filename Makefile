TARGET := myos.bin
ISO := myos.iso
BUILD := build
ISO_ROOT := $(BUILD)/isodir

AS := nasm
CC := gcc
LD := ld
GRUB_FILE := grub-file
GRUB_RES := grub-mkrescue
QEMU := qemu-system-i386

CFLAGS := -m32 -std=gnu99 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -Wall -Wextra
LDFLAGS := -m elf_i386 -T linker.ld
OBJS := $(BUILD)/boot.o $(BUILD)/interrupts.o $(BUILD)/kernel.o $(BUILD)/memory.o $(BUILD)/paging.o $(BUILD)/process.o $(BUILD)/syscalls.o $(BUILD)/user_program.o

.PHONY: all iso run check clean

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	$(GRUB_FILE) --is-x86-multiboot $@

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/interrupts.o: interrupts.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/kernel.o: kernel.c memory.h paging.h process.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/memory.o: memory.c memory.h paging.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/paging.o: paging.c paging.h memory.h linker.ld | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/process.o: process.c process.h memory.h paging.h syscalls.h | $(BUILD)\n\t$(CC) $(CFLAGS) -c $< -o $@\n\n$(BUILD)/syscalls.o: syscalls.c syscalls.h memory.h paging.h process.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/user_program.o: user_program.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

iso: $(TARGET)
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(TARGET) $(ISO_ROOT)/boot/myos.bin
	cp grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUB_RES) -o $(ISO) $(ISO_ROOT)

check: $(TARGET)
	$(GRUB_FILE) --is-x86-multiboot $(TARGET)

run: iso
	$(QEMU) -cdrom $(ISO)

clean:
	rm -rf $(BUILD) $(TARGET) $(ISO)
