TARGET := myos.bin
ISO := myos.iso
DISK := nanoos.disk
DISK_SIZE_MB := 16
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
OBJS := $(BUILD)/boot.o $(BUILD)/interrupts.o $(BUILD)/kernel.o $(BUILD)/terminal.o $(BUILD)/memory.o $(BUILD)/paging.o $(BUILD)/process.o $(BUILD)/elf_loader.o $(BUILD)/syscalls.o $(BUILD)/ata.o $(BUILD)/diskfs.o $(BUILD)/vfs.o $(BUILD)/shell.o $(BUILD)/user_image.o

.PHONY: all iso disk disk-reset run check clean

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

$(BUILD)/kernel.o: kernel.c memory.h paging.h process.h elf.h vfs.h shell.h terminal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/terminal.o: terminal.c terminal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/memory.o: memory.c memory.h paging.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/paging.o: paging.c paging.h memory.h linker.ld | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/process.o: process.c process.h memory.h paging.h syscalls.h elf.h vfs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/elf_loader.o: elf_loader.c elf.h process.h paging.h memory.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/syscalls.o: syscalls.c syscalls.h memory.h paging.h process.h vfs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ata.o: ata.c ata.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/diskfs.o: diskfs.c diskfs.h ata.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vfs.o: vfs.c vfs.h memory.h diskfs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shell.o: shell.c shell.h vfs.h diskfs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/user_program_raw.o: user_program.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user_program.elf: $(BUILD)/user_program_raw.o user.ld | $(BUILD)
	$(LD) -m elf_i386 -T user.ld -o $@ $(BUILD)/user_program_raw.o

$(BUILD)/user_image.o: user_image.asm $(BUILD)/user_program.elf | $(BUILD)
	$(AS) -f elf32 $< -o $@

iso: $(TARGET)
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(TARGET) $(ISO_ROOT)/boot/myos.bin
	cp grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUB_RES) -o $(ISO) $(ISO_ROOT)

disk:
	@if [ ! -f "$(DISK)" ]; then \
		echo "Creating $(DISK_SIZE_MB) MiB NanoOS disk image..."; \
		dd if=/dev/zero of="$(DISK)" bs=1M count=$(DISK_SIZE_MB) status=none; \
	fi

disk-reset:
	rm -f "$(DISK)"

check: $(TARGET)
	$(GRUB_FILE) --is-x86-multiboot $(TARGET)

run: iso disk
	$(QEMU) -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

clean:
	rm -rf $(BUILD) $(TARGET) $(ISO)
