// kernel/main.cpp

#include "vga.hpp"
#include "gdt.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "heap.hpp"
#include "idt.hpp"
#include "pic.hpp"
#include "timer.hpp"
#include "serial.hpp"
#include "keyboard.hpp"
#include "scheduler.hpp"
#include "syscall.hpp"
#include "cpuid.hpp"
#include "vmm.hpp"
#include "pci.hpp"
#include "syscall.hpp"
#include "vfs.hpp"
#include "block.hpp"
#include "ata.hpp"
#include "fat32.hpp"
#include "elf.hpp"
#include "spawn.hpp"

static constexpr uint32_t MULTIBOOT2_MAGIC = 0x36D76289;
extern "C" uint8_t kernel_end[];

static void spurious_irq7()  { pic::eoi(7);  }
static void spurious_irq15() { pic::eoi(15); }

static void klog(const char* msg) {
    vga::write_line(msg);
    serial::write_line(msg);
}

extern "C" void kernel_main(uint32_t mb2_magic, uint32_t mb2_info) {
    vga::init();
    serial::init();

    // ── Banner ────────────────────────────────────────────────────────────
    serial::write_line("--- Knail boot ---");
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    vga::write_line("  _  __              _ _");
    vga::write_line(" | |/ /_ __   __ _  (_) |");
    vga::write_line(" | ' /| '_ \\ / _` | | | |");
    vga::write_line(" | . \\| | | | (_| | | | |");
    vga::write_line(" |_|\\_\\_| |_|\\__,_| |_|_|");
    vga::write_line("");
    vga::set_color(vga::Color::White, vga::Color::Black);
    vga::write_line("Knail v0.1.0  [x86-64]");
    vga::write_line("----------------------");

    // ── Multiboot2 check ──────────────────────────────────────────────────
    if (mb2_magic == MULTIBOOT2_MAGIC) {
        vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        vga::write("[OK] Multiboot2  info=");
        vga::write_hex(mb2_info);
        vga::write_line("");
    } else {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        vga::write("[!!] Bad magic: ");
        vga::write_hex(mb2_magic);
        vga::write_line("");
    }
    
    const cpuid::CpuInfo& cpu = cpuid::detect(); 
    cpuid::print(cpu);
    
    // ── Core hardware init ────────────────────────────────────────────────
    gdt::init();      klog("[OK] GDT + TSS");
    idt::init();      klog("[OK] IDT");
    pic::init(); 
    pci::init();
    pci::dump();
    klog("[OK] PCI");
    
    idt::set_irq_handler(7,  spurious_irq7);
    idt::set_irq_handler(15, spurious_irq15);
    klog("[OK] PIC");
    
    // ── Memory ────────────────────────────────────────────────────────────
    pmm::init(mb2_info, reinterpret_cast<uint64_t>(kernel_end));
    {
        auto s = pmm::stats();
        vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        vga::write("[OK] PMM  ");
        vga::write_dec(s.total_bytes / (1024 * 1024));
        vga::write(" MiB  free=");
        vga::write_dec(s.free_frames * 4);
        vga::write_line(" KiB");
    }
    vmm::init();      klog("[OK] VMM");
    heap::init();     klog("[OK] Heap");

    // ── Filesystems ───────────────────────────────────────────────────────
    vfs::init();      klog("[OK] VFS");
    block::init();
    ata::init();      klog("[OK] ATA");

    vfs::mkdir("/disk");
    block::BlockDevice* hda = block::find("hda");
    if (hda) {
        int64_t r = fat32::mount(hda, "/disk");
        if (r == E_OK) klog("[OK] FAT32 /disk");
        else           klog("[!!] FAT32 mount failed");
    } else {
        klog("[!!] hda not found");
    }

    {
        auto s = pmm::stats();
        serial::write("[PMM] after mount: free=");
        serial::write_dec(s.free_frames);
        serial::write_line("");
    }

    // ── Drivers ───────────────────────────────────────────────────────────
    timer::init(100); klog("[OK] Timer 100Hz");
    keyboard::init(); klog("[OK] Keyboard");

    // ── Kernel services ───────────────────────────────────────────────────
    sched::init();    klog("[OK] Scheduler");
    syscall::init();  klog("[OK] Syscalls");

    // ── Spawn tasks ───────────────────────────────────────────────────────
    __asm__ volatile("cli");

    // ── Launch init process from disk ─────────────────────────────────────
    do {
        elf::Image img = elf::load("/disk/init");
        if (!img.valid) { serial::write_line("Failed to load init!"); break; }

        const char* init_argv[] = {"init", nullptr};
        const char* init_envp[] = {
            "PATH=/disk",
            "TERM=knail",
            nullptr,
        };

        uint32_t uid = sched::spawn_elf(img, "init", 
                init_argv, 1, init_envp, 2);
        if (!uid) { vga::write_line("Failed to start init!"); }
        
        
        elf::Image shell_img = elf::load("/disk/shell");
        if (!shell_img.valid) { serial::write_line("Failed to load init!"); break; }

        const char* shell_argv[] = {"shell", nullptr};
        const char* shell_envp[] = {
            "PATH=/disk",
            "TERM=knail",
            nullptr,
        };

        uint32_t shell_uid = sched::spawn_elf(shell_img, "shell", 
                shell_argv, 1, shell_envp, 2);
        if (!shell_uid) { vga::write_line("Failed to start shell!"); }


    } while (0);
    // ── Go ────────────────────────────────────────────────────────────────
    
        

    __asm__ volatile("sti");
    klog("[OK] Interrupts enabled");
    
    while (true) __asm__ volatile("hlt");
}
