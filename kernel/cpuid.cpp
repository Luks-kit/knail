// kernel/cpuid.cpp
#include "cpuid.hpp"
#include "serial.hpp"
#include "vga.hpp"

namespace cpuid {

static void cpuid_leaf(uint32_t leaf, uint32_t subleaf,
                        uint32_t& eax, uint32_t& ebx,
                        uint32_t& ecx, uint32_t& edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf)
    );
}

static void copy_str(char* dst, uint32_t a, uint32_t b,
                     uint32_t c, uint32_t d, int& pos) {
    auto put = [&](uint32_t v) {
        dst[pos++] = (char)(v & 0xFF);
        dst[pos++] = (char)((v >> 8)  & 0xFF);
        dst[pos++] = (char)((v >> 16) & 0xFF);
        dst[pos++] = (char)((v >> 24) & 0xFF);
    };
    put(a); put(b); put(c); put(d);
}

static CpuInfo cpu_info;
static bool    detected = false;

const CpuInfo& get() {
    return cpu_info;
}

CpuInfo detect() {
    if (detected) return cpu_info;
    cpu_info = {};
    uint32_t eax, ebx, ecx, edx;

    // ── Leaf 0: max leaf + vendor ─────────────────────────────────────────
    cpuid_leaf(0, 0, eax, ebx, ecx, edx);
    uint32_t max_leaf = eax;

    // vendor is EBX EDX ECX (note: NOT EBX ECX EDX)
    char* v = cpu_info.vendor;
    auto put4 = [](char* dst, uint32_t val) {
        dst[0] = (char)(val & 0xFF);
        dst[1] = (char)((val >> 8)  & 0xFF);
        dst[2] = (char)((val >> 16) & 0xFF);
        dst[3] = (char)((val >> 24) & 0xFF);
    };
    put4(v + 0, ebx);
    put4(v + 4, edx);
    put4(v + 8, ecx);
    v[12] = 0;

    // ── Leaf 1: family/model/stepping + features ──────────────────────────
    if (max_leaf >= 1) {
        cpuid_leaf(1, 0, eax, ebx, ecx, edx);

        uint32_t stepping_id  =  eax & 0xF;
        uint32_t model_id     = (eax >> 4)  & 0xF;
        uint32_t family_id    = (eax >> 8)  & 0xF;
        uint32_t ext_model    = (eax >> 16) & 0xF;
        uint32_t ext_family   = (eax >> 20) & 0xFF;

        cpu_info.stepping = (uint8_t)stepping_id;

        if (family_id == 0xF)
            cpu_info.family = (uint8_t)(family_id + ext_family);
        else
            cpu_info.family = (uint8_t)family_id;

        if (family_id == 0x6 || family_id == 0xF)
            cpu_info.model = (uint8_t)((ext_model << 4) | model_id);
        else
            cpu_info.model = (uint8_t)model_id;

        // ECX features
        cpu_info.has_sse3    = (ecx >> 0)  & 1;
        cpu_info.has_ssse3   = (ecx >> 9)  & 1;
        cpu_info.has_sse4_1  = (ecx >> 19) & 1;
        cpu_info.has_sse4_2  = (ecx >> 20) & 1;
        cpu_info.has_x2apic  = (ecx >> 21) & 1;
        cpu_info.has_aes     = (ecx >> 25) & 1;
        cpu_info.has_avx     = (ecx >> 28) & 1;
        cpu_info.has_rdrand  = (ecx >> 30) & 1;

        // EDX features
        cpu_info.has_sse     = (edx >> 25) & 1;
        cpu_info.has_sse2    = (edx >> 26) & 1;
        cpu_info.has_htt     = (edx >> 28) & 1;
        cpu_info.has_apic    = (edx >> 9)  & 1;

        // Logical CPU count from EBX[23:16] (valid if HTT=1)
        cpu_info.logical_cpus = cpu_info.has_htt ? ((ebx >> 16) & 0xFF) : 1;
        if (cpu_info.logical_cpus == 0) cpu_info.logical_cpus = 1;
    }

    // ── Leaf 7: extended features (AVX2) ─────────────────────────────────
    if (max_leaf >= 7) {
        cpuid_leaf(7, 0, eax, ebx, ecx, edx);
        cpu_info.has_avx2 = (ebx >> 5) & 1;
    }

    // ── Extended leaf 0x80000000: max extended + brand string ─────────────
    cpuid_leaf(0x80000000, 0, eax, ebx, ecx, edx);
    uint32_t max_ext = eax;

    if (max_ext >= 0x80000001) {
        cpuid_leaf(0x80000001, 0, eax, ebx, ecx, edx);
        cpu_info.has_nx         = (edx >> 20) & 1;
        cpu_info.has_1gb_pages  = (edx >> 26) & 1;
        cpu_info.has_rdtscp     = (edx >> 27) & 1;
    }

    // Brand string from leaves 0x80000002-0x80000004
    if (max_ext >= 0x80000004) {
        int pos = 0;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid_leaf(leaf, 0, eax, ebx, ecx, edx);
            copy_str(cpu_info.brand, eax, ebx, ecx, edx, pos);
        }
        cpu_info.brand[48] = 0;
    } else {
        cpu_info.brand[0] = '?'; cpu_info.brand[1] = 0;
    }

    // Physical core count: leaf 4 (Intel) or leaf 0x8000001E (AMD)
    cpu_info.phys_cores = 1;
    if (max_leaf >= 4) {
        cpuid_leaf(4, 0, eax, ebx, ecx, edx);
        uint32_t cores = ((eax >> 26) & 0x3F) + 1;
        if (cores > 0) cpu_info.phys_cores = cores;
    }
    detected = true; 

    return cpu_info;
}

void print(const CpuInfo& info) {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    serial::write("[CPU] "); serial::write(info.vendor);
    serial::write(" Family="); serial::write_dec(info.family);
    serial::write(" Model=");  serial::write_dec(info.model);
    serial::write(" Step=");   serial::write_dec(info.stepping);
    serial::write_line("");

    serial::write("[CPU] "); serial::write(info.brand);
    serial::write_line("");

    serial::write("[CPU] Cores=");   serial::write_dec(info.phys_cores);
    serial::write(" Logical=");      serial::write_dec(info.logical_cpus);
    serial::write_line("");

    serial::write("[CPU] Features:");
    if (info.has_sse)     serial::write(" SSE");
    if (info.has_sse2)    serial::write(" SSE2");
    if (info.has_sse3)    serial::write(" SSE3");
    if (info.has_ssse3)   serial::write(" SSSE3");
    if (info.has_sse4_1)  serial::write(" SSE4.1");
    if (info.has_sse4_2)  serial::write(" SSE4.2");
    if (info.has_avx)     serial::write(" AVX");
    if (info.has_avx2)    serial::write(" AVX2");
    if (info.has_aes)     serial::write(" AES");
    if (info.has_rdrand)  serial::write(" RDRAND");
    if (info.has_rdtscp)  serial::write(" RDTSCP");
    if (info.has_nx)      serial::write(" NX");
    if (info.has_apic)    serial::write(" APIC");
    if (info.has_x2apic)  serial::write(" x2APIC");
    if (info.has_1gb_pages) serial::write(" 1GB-pages");
    serial::write_line("");

    // Mirror to VGA
    vga::write("[CPU] "); vga::write(info.vendor);
    vga::write_line("");
    vga::write("[CPU] "); vga::write(info.brand);
    vga::write_line("");
    vga::set_color(vga::Color::White, vga::Color::Black);
}

} // namespace cpuid
