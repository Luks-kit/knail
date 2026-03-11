// kernel/pci.cpp
#include "pci.hpp"
#include "serial.hpp"
#include "vga.hpp"

namespace pci {

static Device dev_list[MAX_DEVICES];
static uint32_t dev_count = 0;

// ── Port I/O ──────────────────────────────────────────────────────────────
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

// ── Config space ──────────────────────────────────────────────────────────
uint32_t config_read(uint8_t bus, uint8_t slot,
                     uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func <<  8)
                  | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void config_write(uint8_t bus, uint8_t slot,
                  uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func <<  8)
                  | (offset & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

// ── Read a full device header ─────────────────────────────────────────────
static bool read_device(uint8_t bus, uint8_t slot,
                         uint8_t func, Device& d) {
    uint32_t id = config_read(bus, slot, func, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return false; // no device

    d.bus  = bus;
    d.slot = slot;
    d.func = func;

    d.vendor_id = (uint16_t)(id & 0xFFFF);
    d.device_id = (uint16_t)(id >> 16);

    uint32_t class_rev = config_read(bus, slot, func, 0x08);
    d.revision  = (uint8_t)(class_rev & 0xFF);
    d.prog_if   = (uint8_t)((class_rev >> 8)  & 0xFF);
    d.subclass  = (uint8_t)((class_rev >> 16) & 0xFF);
    d.class_code= (uint8_t)((class_rev >> 24) & 0xFF);

    uint32_t hdr = config_read(bus, slot, func, 0x0C);
    d.header_type    = (uint8_t)((hdr >> 16) & 0x7F);
    d.multifunction  = !!((hdr >> 16) & 0x80);

    // BARs (only meaningful for header type 0)
    if (d.header_type == 0) {
        for (int i = 0; i < 6; i++)
            d.bar[i] = config_read(bus, slot, func, 0x10 + i * 4);
    } else {
        for (int i = 0; i < 6; i++) d.bar[i] = 0;
    }

    uint32_t irq = config_read(bus, slot, func, 0x3C);
    d.irq_line = (uint8_t)(irq & 0xFF);
    d.irq_pin  = (uint8_t)((irq >> 8) & 0xFF);

    return true;
}

// ── Enumerate ─────────────────────────────────────────────────────────────
static void scan_bus(uint8_t bus);

static void scan_func(uint8_t bus, uint8_t slot, uint8_t func) {
    if (dev_count >= MAX_DEVICES) return;
    Device d;
    if (!read_device(bus, slot, func, d)) return;
    dev_list[dev_count++] = d;

    // If it's a PCI-to-PCI bridge, recurse into the secondary bus
    if (d.class_code == CLASS_BRIDGE && d.subclass == SUB_BRIDGE_PCI) {
        uint32_t buses = config_read(bus, slot, func, 0x18);
        uint8_t secondary = (uint8_t)((buses >> 8) & 0xFF);
        if (secondary != 0) scan_bus(secondary);
    }
}

static void scan_slot(uint8_t bus, uint8_t slot) {
    uint32_t id = config_read(bus, slot, 0, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return; // nothing here

    scan_func(bus, slot, 0);

    // Check if multifunction
    uint32_t hdr = config_read(bus, slot, 0, 0x0C);
    if ((hdr >> 16) & 0x80) {
        for (uint8_t func = 1; func < 8; func++)
            scan_func(bus, slot, func);
    }
}

static void scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++)
        scan_slot(bus, slot);
}

void init() {
    dev_count = 0;

    // Check if host bridge is multifunction — if so, multiple PCI domains
    uint32_t hdr = config_read(0, 0, 0, 0x0C);
    if (!((hdr >> 16) & 0x80)) {
        // Single PCI host controller
        scan_bus(0);
    } else {
        // Multiple host controllers — each func is a separate bus
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = config_read(0, 0, func, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) break;
            scan_bus(func);
        }
    }
}

// ── Lookup ────────────────────────────────────────────────────────────────
const Device* find_class(uint8_t class_code, uint8_t subclass) {
    for (uint32_t i = 0; i < dev_count; i++)
        if (dev_list[i].class_code == class_code &&
            dev_list[i].subclass   == subclass)
            return &dev_list[i];
    return nullptr;
}

const Device* find_device(uint16_t vendor, uint16_t device_id) {
    for (uint32_t i = 0; i < dev_count; i++)
        if (dev_list[i].vendor_id == vendor &&
            dev_list[i].device_id == device_id)
            return &dev_list[i];
    return nullptr;
}

const Device* devices()      { return dev_list; }
uint32_t      device_count() { return dev_count; }

// ── Dump ──────────────────────────────────────────────────────────────────
static const char* class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
        case CLASS_STORAGE:
            switch (sub) {
                case SUB_STORAGE_IDE:  return "IDE controller";
                case SUB_STORAGE_SATA: return "SATA controller";
                case SUB_STORAGE_NVME: return "NVMe controller";
                default:               return "Storage controller";
            }
        case CLASS_NETWORK:   return "Network controller";
        case CLASS_DISPLAY:   return "Display controller";
        case CLASS_BRIDGE:
            switch (sub) {
                case SUB_BRIDGE_HOST: return "Host bridge";
                case SUB_BRIDGE_ISA:  return "ISA bridge";
                case SUB_BRIDGE_PCI:  return "PCI-PCI bridge";
                default:              return "Bridge";
            }
        case CLASS_SERIAL_BUS:
            if (sub == SUB_USB)   return "USB controller";
            return "Serial bus controller";
        case CLASS_INPUT:     return "Input controller";
        case CLASS_MULTIMEDIA:return "Multimedia controller";
        default:              return "Unknown";
    }
}

void dump() {
    serial::write("[PCI] Found ");
    serial::write_dec(dev_count);
    serial::write_line(" devices:");

    for (uint32_t i = 0; i < dev_count; i++) {
        const Device& d = dev_list[i];
        serial::write("  [");
        serial::write_dec(d.bus);  serial::write(":");
        serial::write_dec(d.slot); serial::write(".");
        serial::write_dec(d.func); serial::write("] ");
        serial::write_hex((uint32_t)d.vendor_id);
        serial::write(":");
        serial::write_hex((uint32_t)d.device_id);
        serial::write("  ");
        serial::write(class_name(d.class_code, d.subclass));
        if (d.irq_line != 0xFF) {
            serial::write("  IRQ=");
            serial::write_dec(d.irq_line);
        }
        serial::write_line("");
    }

    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    vga::write("[PCI] ");
    vga::write_dec(dev_count);
    vga::write_line(" devices found");
    vga::set_color(vga::Color::White, vga::Color::Black);
}

} // namespace pci
