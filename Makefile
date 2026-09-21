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

CFLAGS := -m32 -std=gnu99 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra
LDFLAGS := -m elf_i386 -T linker.ld
OBJS := $(BUILD)/boot.o $(BUILD)/interrupts.o $(BUILD)/kernel.o

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

$(BUILD)/kernel.o: kernel.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

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
