# Knail 

<div style="text-align: center; margin: 30px 0;">
  <h2> A Modern x86_64 Kernel Project</h2>
  <p><em>Building a robust operating system from the ground up</em></p>
</div>

---

##  Overview

KNAIL is a **x86_64 architecture operating system kernel** developed from scratch. This project demonstrates comprehensive kernel development skills including memory
management, process scheduling, device drivers, filesystem support, and bootloader configuration.

---

##  Architecture

| Component | Description |
|-----------|-------------|
| **Architecture** | x86_64 (64-bit) |
| **Bootloader** | GRUB |
| **Memory Manager** | Physical Memory Manager (PMM), VMM |
| **Process Scheduler** | Custom scheduler with syscall support |
| **Filesystem** | VFS with FAT32 support |
| **Kernel Language** | C++ with C hybrid components |

---

##  Project Structure

```
knail/
├── arch/                 # Architecture-specific code
│   └── x86_64/
│       ├── include/      # Headers (CPUID, GDT, IDT, etc.)
│       ├── pmm.cpp       # Physical Memory Manager
│       └── vmm.cpp       # Virtual Memory Manager
├── boot/                 # Boot code
│   ├── boot.asm          # Boot sector
│   ├── gdt.asm           # Global Descriptor Table
│   └── idt.asm           # Interrupt Descriptor Table
├── drivers/              # Device Drivers
│   ├── ata.cpp           # ATA/SATA driver
│   ├── keyboard.cpp      # Keyboard driver
│   ├── rtc.cpp           # Real-time clock
│   ├── vga.cpp           # VGA graphics
│   └── include/          # Driver headers
├── fs/                   # Filesystem Implementation
│   ├── fat32.cpp         # FAT32 filesystem
│   ├── vfs.cpp           # Virtual Filesystem
│   └── include/          # Filesystem headers
├── kernel/               # Core Kernel
│   ├── asm/              # Assembly (scheduler, syscall)
│   ├── heap.cpp          # Heap allocator
│   ├── mutex.cpp         # Mutex implementation
│   ├── scheduler.cpp     # Process scheduler
│   ├── timer.cpp         # Timer driver
│   └── include/          # Kernel headers
├── proc/                 # Process Management
│   ├── elf.cpp           # ELF support
│   ├── fdtable.cpp       # File descriptor table
│   └── spawn.cpp         # Process spawning
├── shell/                # User Shell
│   └── shell.c           # Shell implementation
├── init/                 # Initialization
│   └── init.c            # System initialization
├── build/                # Build artifacts
├── scripts/              # Build scripts
│   ├── grub.cfg          # GRUB configuration
│   └── knail.ld          # Linker script
├── Makefile              # Build configuration
├── knail.iso             # Distribution ISO
└── knail.map             # Memory map
```

---

##  Build Instructions

### Prerequisites

```bash
make
```

### Build System

- **Build Tool**: `make`
- **Configuration**: `Makefile` with `compile_commands.json`
- **Linker**: Custom linker script `knail.ld`
- **Kernel Image**: `knail.elf`
- **Distribution ISO**: `knail.iso`

---

##  Features

### Core Components

-  **Memory Management**: Physical memory manager (PMM) & Virtual memory manager (VMM)
-  **Process Scheduling**: Custom scheduler with syscall support
-  **Device Drivers**: ATA, Keyboard, VGA, RTC, PCI, Serial
-  **Filesystems**: VFS layer with FAT32 support
-  **Process Management**: ELF support, file descriptors, process spawning
-  **Synchronization**: Heap allocator with mutex primitives
-  **Boot System**: GRUB bootloader with custom kernel

### Architecture Support

- **x86_64** native support
- **GDT/IDT** initialization
- **CPUID** support for processor information
- **PIC/IOAPIC** timer and interrupt management

---

## Testing & Development

### Test Suite

```bash
./test               # Run kernel tests
```

### Debugging

```bash
knail.map            # Memory map for debugging
knail.elf            # Kernel ELF for analysis
```

---

## License

```

```

---

##  Contributing

1. **Fork** the repository
2. **Create** a feature branch (`git checkout -b feature/amazing-feature`)
3. **Commit** your changes (`git commit -m 'Add amazing feature'`)
4. **Push** to the branch (`git push origin feature/amazing-feature`)
5. **Open** a Pull Request

**Code Style**: Follow existing kernel conventions  
**Testing**: Ensure all tests pass before submitting

---

##  Contact

For questions or contributions, please open an issue in the repository.

---

##  Notes

- This is a **learning and research project**
- Kernel is in **early development**
- Build artifacts are in `build/` directory
- ISO can be created with `knail.iso`

---

<div style="margin-top: 40px; padding: 20px; background: #f7f7f7; border-radius: 8px;">
  <strong> Disclaimer</strong><br>
  This kernel is experimental. Use at your own risk. Not for production systems.
</div>

---

*Last updated: 2026 | Built with nothing worth copying*
