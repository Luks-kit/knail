#pragma once
// include/pci.hpp - PCI bus enumerator
#include <stdint.h>

namespace pci {

// ── PCI config space header (type 0 — normal device) ─────────────────────
struct Device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;

    uint8_t  header_type;   // 0x00=normal, 0x01=bridge, 0x02=cardbus
    bool     multifunction;

    uint8_t  irq_line;
    uint8_t  irq_pin;

    uint32_t bar[6];        // Base Address Registers
};

static constexpr uint32_t MAX_DEVICES = 256;

// ── Class codes ───────────────────────────────────────────────────────────
static constexpr uint8_t CLASS_UNCLASSIFIED  = 0x00;
static constexpr uint8_t CLASS_STORAGE       = 0x01;
static constexpr uint8_t CLASS_NETWORK       = 0x02;
static constexpr uint8_t CLASS_DISPLAY       = 0x03;
static constexpr uint8_t CLASS_MULTIMEDIA    = 0x04;
static constexpr uint8_t CLASS_MEMORY        = 0x05;
static constexpr uint8_t CLASS_BRIDGE        = 0x06;
static constexpr uint8_t CLASS_SERIAL        = 0x07;
static constexpr uint8_t CLASS_INPUT         = 0x09;
static constexpr uint8_t CLASS_SERIAL_BUS    = 0x0C; // USB lives here

// ── Subclasses ────────────────────────────────────────────────────────────
// Storage (0x01)
static constexpr uint8_t SUB_STORAGE_IDE     = 0x01;
static constexpr uint8_t SUB_STORAGE_SATA    = 0x06;
static constexpr uint8_t SUB_STORAGE_NVME    = 0x08;
// Serial bus (0x0C)
static constexpr uint8_t SUB_USB_UHCI        = 0x00;
static constexpr uint8_t SUB_USB_OHCI        = 0x10; // prog_if
static constexpr uint8_t SUB_USB             = 0x03;
static constexpr uint8_t SUB_USB_XHCI_PROGIF = 0x30; // prog_if for xHCI
// Bridge (0x06)
static constexpr uint8_t SUB_BRIDGE_HOST     = 0x00;
static constexpr uint8_t SUB_BRIDGE_ISA      = 0x01;
static constexpr uint8_t SUB_BRIDGE_PCI      = 0x04;

// ── API ───────────────────────────────────────────────────────────────────

// Scan all buses/slots/functions and populate internal device list.
void init();

// Raw config space read — 32-bit aligned.
uint32_t config_read(uint8_t bus, uint8_t slot,
                     uint8_t func, uint8_t offset);

// Raw config space write.
void config_write(uint8_t bus, uint8_t slot,
                  uint8_t func, uint8_t offset, uint32_t val);

// Find first device matching class+subclass. Returns nullptr if not found.
const Device* find_class(uint8_t class_code, uint8_t subclass);

// Find device by vendor+device ID. Returns nullptr if not found.
const Device* find_device(uint16_t vendor, uint16_t device_id);

// Iterate all found devices.
const Device* devices();
uint32_t      device_count();

// Print all devices to serial.
void dump();

} // namespace pci
