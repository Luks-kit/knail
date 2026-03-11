#pragma once
// include/cpuid.hpp - x86-64 CPUID scanner
#include <stdint.h>

namespace cpuid {

struct CpuInfo {
    // Vendor + brand
    char vendor[13];        // e.g. "GenuineIntel"
    char brand[49];         // e.g. "Intel(R) Core(TM) i7..."

    // Family/model/stepping
    uint8_t family;
    uint8_t model;
    uint8_t stepping;

    // Topology
    uint32_t logical_cpus;  // hyperthreads per package
    uint32_t phys_cores;    // physical cores per package

    // Feature flags
    bool has_apic;
    bool has_x2apic;
    bool has_htt;
    bool has_sse;
    bool has_sse2;
    bool has_sse3;
    bool has_ssse3;
    bool has_sse4_1;
    bool has_sse4_2;
    bool has_avx;
    bool has_avx2;
    bool has_aes;
    bool has_rdrand;
    bool has_nx;            // from extended leaf
    bool has_1gb_pages;     // from extended leaf
    bool has_rdtscp;
};

// Run CPUID and populate a CpuInfo struct.
CpuInfo detect();

// Call once at boot. Returns the cached result.
const CpuInfo& get();

// Print everything to serial + VGA.
void print(const CpuInfo& info);

} // namespace cpuid
