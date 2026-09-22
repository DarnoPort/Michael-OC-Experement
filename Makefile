TARGET := myos.bin
ISO := myos.iso
DISK := michaelos.disk
DISK_SIZE_MB := 16
BUILD := build
ISO_ROOT := $(BUILD)/isodir

include version.mk
VERSION_HEADER := $(BUILD)/version.h
VERSION_GRUB := $(BUILD)/grub.cfg

AS := nasm
CC := gcc
LD := ld
GRUB_FILE := grub-file
GRUB_RES := grub-mkrescue
QEMU := qemu-system-i386

CFLAGS := -m32 -std=gnu99 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-builtin -Wall -Wextra
LDFLAGS := -m elf_i386 -T linker.ld
OBJS := $(BUILD)/boot.o $(BUILD)/interrupts.o $(BUILD)/kernel.o $(BUILD)/terminal.o $(BUILD)/terminal_font.o $(BUILD)/memory.o $(BUILD)/paging.o $(BUILD)/process.o $(BUILD)/elf_loader.o $(BUILD)/syscalls.o $(BUILD)/ata.o $(BUILD)/diskfs.o $(BUILD)/vfs.o $(BUILD)/shell.o $(BUILD)/user_image.o $(BUILD)/user_exec_image.o $(BUILD)/user_args_image.o $(BUILD)/user_stdio_image.o

.PHONY: all iso disk disk-reset disk-import disk-export disk-ls elf-install elf-check disk-elf-check run check clean

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld $(VERSION_HEADER)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	$(GRUB_FILE) --is-x86-multiboot $@

$(VERSION_HEADER): version.mk | $(BUILD)
	printf '%s\n' '#ifndef MICHAEL_OS_VERSION_H' '#define MICHAEL_OS_VERSION_H' '' '#define MICHAEL_OS_VERSION_STRING "$(MICHAEL_OS_VERSION)"' '#endif' > $@

$(VERSION_GRUB): grub.cfg.in version.mk | $(BUILD)
	sed 's/@VERSION@/$(MICHAEL_OS_VERSION)/g' grub.cfg.in > $@

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/interrupts.o: interrupts.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/kernel.o: kernel.c memory.h paging.h process.h elf.h vfs.h shell.h terminal.h $(VERSION_HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -c $< -o $@

$(BUILD)/terminal.o: terminal.c terminal.h terminal_font.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/terminal_font.o: terminal_font.c terminal_font.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/memory.o: memory.c memory.h paging.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/paging.o: paging.c paging.h memory.h linker.ld | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/process.o: process.c process.h memory.h paging.h syscalls.h elf.h vfs.h terminal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/elf_loader.o: elf_loader.c elf.h process.h paging.h memory.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/syscalls.o: syscalls.c syscalls.h memory.h paging.h process.h vfs.h terminal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ata.o: ata.c ata.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/diskfs.o: diskfs.c diskfs.h ata.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vfs.o: vfs.c vfs.h memory.h diskfs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/shell.o: shell.c shell.h vfs.h diskfs.h terminal.h process.h memory.h $(VERSION_HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -c $< -o $@

$(BUILD)/user_args_raw.o: user_args.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user_args.elf: $(BUILD)/user_args_raw.o user.ld | $(BUILD)
	$(LD) -m elf_i386 -T user.ld -o $@ $(BUILD)/user_args_raw.o

$(BUILD)/user_args_image.o: user_args_image.asm $(BUILD)/user_args.elf | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user_stdio_raw.o: user_stdio.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user_stdio.elf: $(BUILD)/user_stdio_raw.o user.ld | $(BUILD)
	$(LD) -m elf_i386 -T user.ld -o $@ $(BUILD)/user_stdio_raw.o

$(BUILD)/user_stdio_image.o: user_stdio_image.asm $(BUILD)/user_stdio.elf | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user_program_raw.o: user_program.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user_program.elf: $(BUILD)/user_program_raw.o user.ld | $(BUILD)
	$(LD) -m elf_i386 -T user.ld -o $@ $(BUILD)/user_program_raw.o

$(BUILD)/user_image.o: user_image.asm $(BUILD)/user_program.elf | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user_exec_raw.o: user_exec.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/user_exec.elf: $(BUILD)/user_exec_raw.o user.ld | $(BUILD)
	$(LD) -m elf_i386 -T user.ld -o $@ $(BUILD)/user_exec_raw.o

$(BUILD)/user_exec_image.o: user_exec_image.asm $(BUILD)/user_exec.elf | $(BUILD)
	$(AS) -f elf32 $< -o $@

iso: $(TARGET) $(VERSION_GRUB)
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(TARGET) $(ISO_ROOT)/boot/myos.bin
	cp $(VERSION_GRUB) $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUB_RES) -o $(ISO) $(ISO_ROOT)

disk:
	@if [ ! -f "$(DISK)" ] && [ -f "nanoos.disk" ]; then \
		echo "Migrating nanoos.disk -> $(DISK)..."; \
		mv nanoos.disk "$(DISK)"; \
	fi
	@if [ ! -f "$(DISK)" ]; then \
		echo "Creating $(DISK_SIZE_MB) MiB Michael OS disk image..."; \
	dd if=/dev/zero of="$(DISK)" bs=1M count=$(DISK_SIZE_MB) status=none; \
	fi

disk-reset:
	rm -f "$(DISK)" nanoos.disk

elf-install:
	@test -n "$(FILE)" || (echo "Usage: make elf-install FILE=host.elf DEST=/bin/program.elf"; exit 1)
	@test -n "$(DEST)" || (echo "Usage: make elf-install FILE=host.elf DEST=/bin/program.elf"; exit 1)
	python3 tools/diskfs_host.py --disk "$(DISK)" install-elf "$(FILE)" "$(DEST)"

elf-check:
	@test -n "$(FILE)" || (echo "Usage: make elf-check FILE=host.elf"; exit 1)
	python3 tools/diskfs_host.py elf-check "$(FILE)"

disk-elf-check:
	@test -n "$(SRC)" || (echo "Usage: make disk-elf-check SRC=/bin/program.elf"; exit 1)
	python3 tools/diskfs_host.py --disk "$(DISK)" disk-elf-check "$(SRC)"

disk-import:
	@test -n "$(FILE)" || (echo "Usage: make disk-import FILE=host_file DEST=/disk/path"; exit 1)
	@test -n "$(DEST)" || (echo "Usage: make disk-import FILE=host_file DEST=/disk/path"; exit 1)
	python3 tools/diskfs_host.py --disk "$(DISK)" import "$(FILE)" "$(DEST)"

disk-export: disk
	@test -n "$(SRC)" || (echo "Usage: make disk-export SRC=/disk/path FILE=host_file"; exit 1)
	@test -n "$(FILE)" || (echo "Usage: make disk-export SRC=/disk/path FILE=host_file"; exit 1)
	python3 tools/diskfs_host.py --disk "$(DISK)" export "$(SRC)" "$(FILE)"

disk-ls: disk
	python3 tools/diskfs_host.py --disk "$(DISK)" ls "$(DISK_PATH)"

check: $(TARGET)
	$(GRUB_FILE) --is-x86-multiboot $(TARGET)

run: iso disk
	$(QEMU) -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

clean:
	rm -rf $(BUILD) $(TARGET) $(ISO)
