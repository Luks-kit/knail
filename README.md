# knail 🖥️

A minimal x86-64 hobby kernel written in C++, C, and Assembly.

## Features
- **Multiboot2** bootloader protocol (works with GRUB)
- **Long mode** (64-bit) entry via a hand-rolled bootstrap
- **VGA text-mode** driver — colours, scrolling, hex/decimal printing
- **Physical Memory Manager** — bitmap allocator, page alloc/free

## Project layout

```
knail/
├── boot/
│   └── boot.asm        # Multiboot2 header, page tables, GDT, long-mode jump
├── kernel/
│   ├── main.cpp        # kernel_main — ties everything together
│   └── vga.cpp         # 80×25 VGA text driver
├── mm/
│   └── pmm.cpp         # Bitmap physical page allocator
├── include/
│   ├── vga.h
│   └── mm.h
├── kernel.ld           # Linker script (loads at 1 MiB)
└── Makefile
```

## Prerequisites

### Debian / Ubuntu
```bash
sudo apt install nasm grub-pc-bin grub-common xorriso qemu-system-x86
# Cross-compiler (recommended):
sudo apt install gcc-x86-64-linux-gnu g++-x86-64-linux-gnu binutils-x86-64-linux-gnu
```

### Arch Linux
```bash
sudo pacman -S nasm grub xorriso qemu-system-x86
# Cross-compiler:
sudo pacman -S cross-x86_64-elf-gcc cross-x86_64-elf-binutils
```

### macOS (Homebrew)
```bash
brew install nasm x86_64-elf-gcc x86_64-elf-binutils grub xorriso qemu
```

## Build & run

```bash
# Build the kernel ELF
make

# Build a bootable ISO
make iso

# Launch in QEMU (boots from ISO)
make run

# Quick launch without ISO (QEMU direct ELF — QEMU ≥ 5.x)
make run-elf
```


