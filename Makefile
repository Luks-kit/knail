# Makefile – Knail kernel build system
# Requires: x86_64-elf cross-compiler (gcc/g++/ld) + nasm + grub-mkrescue + xorriso

# ── Toolchain ─────────────────────────────────────────────────────────────
CROSS   ?= x86_64-elf
CC       = $(CROSS)-gcc
CXX      = $(CROSS)-g++
LD       = $(CROSS)-ld
ASM      = nasm

# ── Directories ───────────────────────────────────────────────────────────
BOOT_DIR    = boot
ARCH_DIR    = arch/x86_64
KERNEL_DIR  = kernel
DRIVER_DIR  = drivers
FS_DIR      = fs
PROC_DIR    = proc
ASM_DIR		= asm
SCRIPTS_DIR = scripts
BUILD_DIR   = build
ISO_DIR     = iso

# ── Output ────────────────────────────────────────────────────────────────
KERNEL = $(BUILD_DIR)/knail.elf
ISO    = knail.iso

# ── Include paths (one -I per module's include/ dir) ─────────────────────
INCLUDES := \
    -I $(KERNEL_DIR)/include \
    -I $(DRIVER_DIR)/include \
    -I $(ARCH_DIR)/include \
    -I $(FS_DIR)/include \
    -I $(PROC_DIR)/include

# ── Flags ─────────────────────────────────────────────────────────────────
ASMFLAGS = -f elf64 -F dwarf -g

COMMON_FLAGS = \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic \
    -mno-red-zone \
    -mno-mmx -mno-sse -mno-sse2 \
    -Wall -Wextra \
    -g \
    $(INCLUDES)

CFLAGS   = -std=c11   $(COMMON_FLAGS)
CXXFLAGS = -std=c++17 $(COMMON_FLAGS) \
    -fno-exceptions \
    -fno-rtti

LDFLAGS = -T $(SCRIPTS_DIR)/knail.ld -Map=knail.map \
    -nostdlib \
    -z max-page-size=0x1000

# ── Sources ───────────────────────────────────────────────────────────────
ASM_SRCS := $(wildcard $(BOOT_DIR)/*.asm) \
			$(wildcard $(KERNEL_DIR)/$(ASM_DIR)/*.asm)

C_SRCS   := $(wildcard $(KERNEL_DIR)/*.c) \
            $(wildcard $(DRIVER_DIR)/*.c) \
            $(wildcard $(ARCH_DIR)/*.c) \
            $(wildcard $(FS_DIR)/*.c) \
            $(wildcard $(PROC_DIR)/*.c)

CXX_SRCS := $(wildcard $(KERNEL_DIR)/*.cpp) \
            $(wildcard $(DRIVER_DIR)/*.cpp) \
            $(wildcard $(ARCH_DIR)/*.cpp) \
            $(wildcard $(FS_DIR)/*.cpp) \
            $(wildcard $(PROC_DIR)/*.cpp)

# ── Objects ───────────────────────────────────────────────────────────────
ASM_OBJS := $(patsubst %.asm, $(BUILD_DIR)/%.o, $(ASM_SRCS))
C_OBJS   := $(patsubst %.c,   $(BUILD_DIR)/%.o, $(C_SRCS))
CXX_OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(CXX_SRCS))
ALL_OBJS  = $(ASM_OBJS) $(C_OBJS) $(CXX_OBJS)

# ── Default target ────────────────────────────────────────────────────────
.PHONY: all iso run run-elf run-gdb debug clean

all: $(KERNEL)

# ── Link ──────────────────────────────────────────────────────────────────
$(KERNEL): $(ALL_OBJS) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $^
	@echo "  LD   $@"

# ── Assemble ──────────────────────────────────────────────────────────────
$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@
	@echo "  ASM  $<"

# ── Compile C ─────────────────────────────────────────────────────────────
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "  CC   $<"

# ── Compile C++ ───────────────────────────────────────────────────────────
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
	@echo "  CXX  $<"

# ── Disk image ────────────────────────────────────────────────────────────
DISK_IMG     = disk.img
DISK_SIZE_MB = 64

disk.img:
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(DISK_SIZE_MB)
	mkfs.fat -F32 $(DISK_IMG)

# ── Build a bootable ISO (requires grub-mkrescue + xorriso) ───────────────
iso: $(KERNEL)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/knail.elf
	cp $(SCRIPTS_DIR)/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISO_DIR)
	@echo "  ISO  $(ISO)"

# ── Run in QEMU ───────────────────────────────────────────────────────────
run: iso disk.img
	qemu-system-x86_64 \
		-boot order=d \
		-drive file=$(ISO),if=ide,index=1,media=cdrom \
		-drive file=$(DISK_IMG),format=raw,if=ide,index=0 \
		-m 128M \
		-serial stdio \
		-device VGA,vgamem_mb=16

# ── Debug run — logs all interrupts/exceptions ────────────────────────────
debug: iso disk.img
	qemu-system-x86_64 \
		-boot order=d \
		-drive file=$(ISO),if=ide,index=1,media=cdrom \
		-drive file=$(DISK_IMG),format=raw,if=ide,index=0 \
		-m 128M \
		-serial file:/tmp/knail_serial.log \
		-device VGA,vgamem_mb=16 \
		-d int,cpu_reset -D /tmp/knail_int.log \
		-no-reboot; \
	echo "=== LAST 60 INTERRUPT LOG LINES ==="; \
	tail -60 /tmp/knail_int.log

# ── Run ELF directly (no ISO needed) ─────────────────────────────────────
run-elf: $(KERNEL)
	qemu-system-x86_64 -kernel $(KERNEL) -m 128M -serial stdio

# ── GDB stub ──────────────────────────────────────────────────────────────
run-gdb: iso disk.img
	qemu-system-x86_64 \
		-s -S \
		-boot order=d \
		-drive file=$(ISO),if=ide,index=1,media=cdrom \
		-drive file=$(DISK_IMG),format=raw,if=ide,index=0 \
		-m 128M \
		-serial stdio

# ── Clean ─────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR) $(ISO)
