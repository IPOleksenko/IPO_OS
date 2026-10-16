#include <kernel/smp.h>
#include <memory/kmalloc.h>
#include <memory/memory_map.h>
#include <stdio.h>
#include <string.h>
#include <ioport.h>

#define LAPIC_DEFAULT_BASE   0xFEE00000u
#define LAPIC_ID_REG         0x0020u
#define LAPIC_TPR_REG        0x0080u
#define LAPIC_EOI_REG        0x00B0u
#define LAPIC_SVR_REG        0x00F0u
#define LAPIC_ICR_LOW        0x0300u
#define LAPIC_ICR_HIGH       0x0310u
#define LAPIC_LINT0_REG      0x0350u
#define LAPIC_LINT1_REG      0x0360u

#define IA32_APIC_BASE_MSR   0x1Bu
#define APIC_BASE_ENABLE_BIT (1u << 11)

#define SMP_TRAMPOLINE_ADDR  0x8000u
#define SMP_BOOT_STACK_ADDR  0x80F0u
#define SMP_BOOT_CPUID_ADDR  0x80F4u
#define SMP_BOOT_FN_ADDR     0x80F8u
#define SMP_BOOT_DONE_ADDR   0x80FCu

#define SMP_IPI_VECTOR       0xE0u

/* Hardware on-wire descriptor sizes */
#define ACPI_RSDP_HW_SIZE            36
#define ACPI_RSDP_V1_HW_SIZE         20
#define ACPI_SDT_HEADER_HW_SIZE      36
#define ACPI_MADT_HW_SIZE            (ACPI_SDT_HEADER_HW_SIZE + 8)
#define MP_FLOATING_POINTER_HW_SIZE  16

/* Dynamic ACPI RSDP Descriptor (no fixed array limits) */
typedef struct {
    char     *signature;
    size_t   signature_len;
    size_t   signature_cap;
    uint8_t  checksum;
    char     *oem_id;
    size_t   oem_id_len;
    size_t   oem_id_cap;
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  *reserved;
    size_t   reserved_len;
    size_t   reserved_cap;
} acpi_rsdp_t;
typedef acpi_rsdp_t acpi_rsdp_raw_t;

/* Dynamic SDT Header (no fixed array limits) */
typedef struct {
    char     *signature;
    size_t   signature_len;
    size_t   signature_cap;
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     *oem_id;
    size_t   oem_id_len;
    size_t   oem_id_cap;
    char     *oem_table_id;
    size_t   oem_table_id_len;
    size_t   oem_table_id_cap;
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;
typedef acpi_sdt_header_t acpi_sdt_header_raw_t;

/* Dynamic ACPI MADT (no fixed array limits) */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t          lapic_addr;
    uint32_t          flags;
} acpi_madt_t;
typedef acpi_madt_t acpi_madt_raw_t;

/* Dynamic MP Floating Pointer Structure (no fixed array limits) */
typedef struct {
    char     *signature;
    size_t   signature_len;
    size_t   signature_cap;
    uint32_t phys_addr;
    uint8_t  length;
    uint8_t  spec_rev;
    uint8_t  checksum;
    uint8_t  *feature;
    size_t   feature_len;
    size_t   feature_cap;
} mp_floating_pointer_t;
typedef mp_floating_pointer_t mp_floating_pointer_raw_t;

/* Helper to set dynamic strings */
static int smp_dyn_str_set(char **dst, size_t *len, size_t *cap, const char *src, size_t max_src) {
    size_t slen = 0;
    while (slen < max_src && src && src[slen]) slen++;
    if (slen + 1 > *cap || !*dst) {
        char *nb = (char *)krealloc(*dst, slen + 1);
        if (!nb) return -1;
        *dst = nb;
        *cap = slen + 1;
    }
    if (src && slen) memcpy(*dst, src, slen);
    (*dst)[slen] = 0;
    *len = slen;
    return 0;
}

static int smp_dyn_buf_set(uint8_t **dst, size_t *len, size_t *cap, const uint8_t *src, size_t n) {
    if (n > *cap || !*dst) {
        size_t nc = n ? n : 1;
        uint8_t *nb = (uint8_t *)krealloc(*dst, nc);
        if (!nb) return -1;
        *dst = nb;
        *cap = nc;
    }
    if (src && n) memcpy(*dst, src, n);
    *len = n;
    return 0;
}

void acpi_rsdp_init(acpi_rsdp_t *r) { if (r) memset(r, 0, sizeof(*r)); }
void acpi_rsdp_free(acpi_rsdp_t *r) {
    if (!r) return;
    if (r->signature) kfree(r->signature);
    if (r->oem_id) kfree(r->oem_id);
    if (r->reserved) kfree(r->reserved);
    memset(r, 0, sizeof(*r));
}
void acpi_rsdp_from_raw(acpi_rsdp_t *dst, const void *raw_bytes) {
    if (!dst || !raw_bytes) return;
    const uint8_t *b = (const uint8_t *)raw_bytes;
    smp_dyn_str_set(&dst->signature, &dst->signature_len, &dst->signature_cap, (const char *)(b + 0), 8);
    dst->checksum = b[8];
    smp_dyn_str_set(&dst->oem_id, &dst->oem_id_len, &dst->oem_id_cap, (const char *)(b + 9), 6);
    dst->revision = b[15];
    dst->rsdt_addr = *(const uint32_t *)(b + 16);
    if (dst->revision >= 2) {
        dst->length = *(const uint32_t *)(b + 20);
        dst->xsdt_addr = *(const uint64_t *)(b + 24);
        dst->ext_checksum = b[32];
        smp_dyn_buf_set(&dst->reserved, &dst->reserved_len, &dst->reserved_cap, b + 33, 3);
    } else {
        dst->length = ACPI_RSDP_V1_HW_SIZE;
        dst->xsdt_addr = 0;
        dst->ext_checksum = 0;
        smp_dyn_buf_set(&dst->reserved, &dst->reserved_len, &dst->reserved_cap, NULL, 0);
    }
}

void acpi_sdt_header_init(acpi_sdt_header_t *h) { if (h) memset(h, 0, sizeof(*h)); }
void acpi_sdt_header_free(acpi_sdt_header_t *h) {
    if (!h) return;
    if (h->signature) kfree(h->signature);
    if (h->oem_id) kfree(h->oem_id);
    if (h->oem_table_id) kfree(h->oem_table_id);
    memset(h, 0, sizeof(*h));
}
void acpi_sdt_header_from_raw(acpi_sdt_header_t *dst, const void *raw_bytes) {
    if (!dst || !raw_bytes) return;
    const uint8_t *b = (const uint8_t *)raw_bytes;
    smp_dyn_str_set(&dst->signature, &dst->signature_len, &dst->signature_cap, (const char *)(b + 0), 4);
    dst->length = *(const uint32_t *)(b + 4);
    dst->revision = b[8];
    dst->checksum = b[9];
    smp_dyn_str_set(&dst->oem_id, &dst->oem_id_len, &dst->oem_id_cap, (const char *)(b + 10), 6);
    smp_dyn_str_set(&dst->oem_table_id, &dst->oem_table_id_len, &dst->oem_table_id_cap, (const char *)(b + 16), 8);
    dst->oem_revision = *(const uint32_t *)(b + 24);
    dst->creator_id = *(const uint32_t *)(b + 28);
    dst->creator_revision = *(const uint32_t *)(b + 32);
}

void acpi_madt_init(acpi_madt_t *m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    acpi_sdt_header_init(&m->header);
}
void acpi_madt_free(acpi_madt_t *m) {
    if (!m) return;
    acpi_sdt_header_free(&m->header);
    memset(m, 0, sizeof(*m));
}
void acpi_madt_from_raw(acpi_madt_t *dst, const void *raw_bytes) {
    if (!dst || !raw_bytes) return;
    const uint8_t *b = (const uint8_t *)raw_bytes;
    acpi_sdt_header_from_raw(&dst->header, b);
    dst->lapic_addr = *(const uint32_t *)(b + ACPI_SDT_HEADER_HW_SIZE);
    dst->flags = *(const uint32_t *)(b + ACPI_SDT_HEADER_HW_SIZE + 4);
}

void mp_floating_pointer_init(mp_floating_pointer_t *mp) { if (mp) memset(mp, 0, sizeof(*mp)); }
void mp_floating_pointer_free(mp_floating_pointer_t *mp) {
    if (!mp) return;
    if (mp->signature) kfree(mp->signature);
    if (mp->feature) kfree(mp->feature);
    memset(mp, 0, sizeof(*mp));
}
void mp_floating_pointer_from_raw(mp_floating_pointer_t *dst, const void *raw_bytes) {
    if (!dst || !raw_bytes) return;
    const uint8_t *b = (const uint8_t *)raw_bytes;
    smp_dyn_str_set(&dst->signature, &dst->signature_len, &dst->signature_cap, (const char *)(b + 0), 4);
    dst->phys_addr = *(const uint32_t *)(b + 4);
    dst->length = b[8];
    dst->spec_rev = b[9];
    dst->checksum = b[10];
    smp_dyn_buf_set(&dst->feature, &dst->feature_len, &dst->feature_cap, b + 11, 5);
}


static cpu_info_t *cpus = NULL;
static uint32_t cpu_count = 0;
static uint32_t cpu_capacity = 0;
static uint32_t active_cpu_count = 0;
static uint32_t lapic_base_addr = LAPIC_DEFAULT_BASE;

static volatile bool smp_running = true;
static volatile uint32_t smp_ap_boot_done = 0;
static volatile uint32_t *ipi_received_count = NULL;

static bool smp_add_cpu(uint32_t lapic_id, bool is_bsp) {
    if (cpu_count >= cpu_capacity) {
        uint32_t new_cap = (cpu_capacity == 0) ? 8 : (cpu_capacity * 2);
        cpu_info_t *new_cpus = (cpu_info_t *)krealloc(cpus, new_cap * sizeof(cpu_info_t));
        if (!new_cpus) return false;
        cpus = new_cpus;

        volatile uint32_t *new_ipis = (volatile uint32_t *)krealloc((void *)ipi_received_count, new_cap * sizeof(uint32_t));
        if (!new_ipis) return false;
        memset((void *)(new_ipis + cpu_capacity), 0, (new_cap - cpu_capacity) * sizeof(uint32_t));
        ipi_received_count = new_ipis;
        cpu_capacity = new_cap;
    }
    uint32_t id = cpu_count;
    cpus[id].cpu_id = id;
    cpus[id].lapic_id = lapic_id;
    cpus[id].is_bsp = is_bsp;
    cpus[id].is_online = is_bsp;
    cpus[id].stack_base = 0;
    cpus[id].stack_top = 0;
    cpus[id].ticks = 0;
    ipi_received_count[id] = 0;
    cpu_count++;
    return true;
}

extern uint8_t smp_trampoline_start[];
extern uint8_t smp_trampoline_end[];

static struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) s_bsp_idt_ptr;



static smp_architecture_status_t arch_status = {0};

static bool s_has_cpuid = false;
static bool s_has_apic = false;
static bool s_has_htt = false;
static char   *s_cpu_vendor = NULL;
static size_t s_cpu_vendor_len = 0;
static size_t s_cpu_vendor_cap = 0;
static bool s_madt_enumerated = false;
static uint32_t s_madt_cpu_count = 0;
static uint32_t s_madt_addr = 0;
static bool s_mp_table_enumerated = false;
static uint32_t s_mp_cpu_count = 0;
static uint32_t s_mp_table_addr = 0;
static bool s_stacks_allocated = false;
static bool s_atomic_locks_verified = false;

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf));
}

static inline void cpu_get_msr(uint32_t msr, uint32_t *lo, uint32_t *hi) {
    __asm__ volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static inline void cpu_set_msr(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static void smp_delay_us(uint32_t us) {
    for (volatile uint32_t i = 0; i < us; i++) {
        io_wait();
    }
}

static void smp_delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms; i++) {
        smp_delay_us(1000);
    }
}

static bool check_cpuid_supported(void) {
    uint32_t eflags1, eflags2;
    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x00200000, %1\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "pushl %0\n\t"
        "popfl\n\t"
        : "=r"(eflags1), "=r"(eflags2));
    return ((eflags1 ^ eflags2) & 0x00200000) != 0;
}

static uint8_t checksum_calc(const uint8_t *data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static const void *find_acpi_rsdp_raw(void) {
    /* 1. Search first 1KB of EBDA */
    uint16_t ebda_seg = *(const volatile uint16_t *)0x040E;
    uint32_t ebda_addr = ((uint32_t)ebda_seg) << 4;
    if (ebda_addr >= 0x80000 && ebda_addr <= 0x9FFFF) {
        for (uint32_t p = ebda_addr; p < ebda_addr + 1024; p += 16) {
            if (memcmp((const void *)(uintptr_t)p, "RSD PTR ", 8) == 0) {
                if (checksum_calc((const uint8_t *)(uintptr_t)p, 20) == 0) {
                    return (const void *)(uintptr_t)p;
                }
            }
        }
    }

    /* 2. Search BIOS ROM area 0x000E0000 - 0x000FFFFF */
    for (uint32_t p = 0x000E0000u; p < 0x00100000u; p += 16) {
        if (memcmp((const void *)(uintptr_t)p, "RSD PTR ", 8) == 0) {
            if (checksum_calc((const uint8_t *)(uintptr_t)p, 20) == 0) {
                return (const void *)(uintptr_t)p;
            }
        }
    }

    return NULL;
}

static bool parse_acpi_madt(void) {
    const void *raw_rsdp = find_acpi_rsdp_raw();
    if (!raw_rsdp) {
        serial_printf("[smp] ACPI RSDP not found\n");
        return false;
    }

    acpi_rsdp_t rsdp;
    acpi_rsdp_init(&rsdp);
    acpi_rsdp_from_raw(&rsdp, raw_rsdp);

    serial_printf("[smp] Found ACPI RSDP at 0x%x (OEM: %s)\n", (uint32_t)(uintptr_t)raw_rsdp, rsdp.oem_id ? rsdp.oem_id : "");

    const uint8_t *raw_rsdt = (const uint8_t *)(uintptr_t)rsdp.rsdt_addr;
    if (!raw_rsdt || memcmp(raw_rsdt, "RSDT", 4) != 0) {
        serial_printf("[smp] Invalid RSDT at 0x%x\n", rsdp.rsdt_addr);
        acpi_rsdp_free(&rsdp);
        return false;
    }

    acpi_sdt_header_t rsdt;
    acpi_sdt_header_init(&rsdt);
    acpi_sdt_header_from_raw(&rsdt, raw_rsdt);

    if (checksum_calc(raw_rsdt, rsdt.length) != 0) {
        serial_printf("[smp] Invalid RSDT checksum at 0x%x\n", rsdp.rsdt_addr);
        acpi_sdt_header_free(&rsdt);
        acpi_rsdp_free(&rsdp);
        return false;
    }

    uint32_t entry_count = (rsdt.length > ACPI_SDT_HEADER_HW_SIZE) ? ((rsdt.length - ACPI_SDT_HEADER_HW_SIZE) / sizeof(uint32_t)) : 0;
    const uint32_t *tables = (const uint32_t *)(raw_rsdt + ACPI_SDT_HEADER_HW_SIZE);

    const void *raw_madt = NULL;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *tbl = (const uint8_t *)(uintptr_t)tables[i];
        if (tbl && memcmp(tbl, "APIC", 4) == 0) {
            uint32_t tbl_len = *(const uint32_t *)(tbl + 4);
            if (checksum_calc(tbl, tbl_len) == 0) {
                raw_madt = tbl;
                break;
            }
        }
    }

    if (!raw_madt) {
        serial_printf("[smp] MADT (APIC table) not found in RSDT\n");
        acpi_sdt_header_free(&rsdt);
        acpi_rsdp_free(&rsdp);
        return false;
    }

    acpi_madt_t madt;
    acpi_madt_init(&madt);
    acpi_madt_from_raw(&madt, raw_madt);

    s_madt_addr = (uint32_t)(uintptr_t)raw_madt;
    lapic_base_addr = madt.lapic_addr ? madt.lapic_addr : LAPIC_DEFAULT_BASE;
    serial_printf("[smp] Found ACPI MADT at 0x%x, LAPIC Base = 0x%x\n",
                  (uint32_t)(uintptr_t)raw_madt, lapic_base_addr);

    /* Enumerate MADT entries */
    const uint8_t *ptr = (const uint8_t *)raw_madt + ACPI_MADT_HW_SIZE;
    const uint8_t *end = (const uint8_t *)raw_madt + madt.header.length;
    uint32_t prev_cpu_count = cpu_count;

    while (ptr < end) {
        uint8_t type = ptr[0];
        uint8_t len = ptr[1];
        if (len < 2 || ptr + len > end) break;

        if (type == 0 /* Processor Local APIC */) {
            uint8_t acpi_id = ptr[2];
            uint8_t apic_id = ptr[3];
            uint32_t flags = *(const uint32_t *)(ptr + 4);

            bool enabled = (flags & 1) != 0;
            serial_printf("[smp]   ACPI Processor ID=%u, APIC ID=%u, Enabled=%s\n",
                          acpi_id, apic_id, enabled ? "yes" : "no");

            if (enabled) {
                smp_add_cpu(apic_id, (cpu_count == 0));
            }
        }
        ptr += len;
    }

    acpi_madt_free(&madt);
    acpi_sdt_header_free(&rsdt);
    acpi_rsdp_free(&rsdp);

    if (cpu_count > prev_cpu_count) {
        s_madt_enumerated = true;
        s_madt_cpu_count = cpu_count - prev_cpu_count;
        arch_status.acpi_madt_enumeration = SMP_STATUS_ACTIVE;
        return true;
    }

    arch_status.acpi_madt_enumeration = SMP_STATUS_INACTIVE;
    return false;
}

static bool parse_mp_table(void) {
    /* Scan 0x000F0000 - 0x000FFFFF for "_MP_" */
    const void *raw_mp = NULL;
    for (uint32_t p = 0x000F0000u; p < 0x00100000u; p += 16) {
        if (memcmp((const void *)(uintptr_t)p, "_MP_", 4) == 0) {
            if (checksum_calc((const uint8_t *)(uintptr_t)p, MP_FLOATING_POINTER_HW_SIZE) == 0) {
                raw_mp = (const void *)(uintptr_t)p;
                break;
            }
        }
    }

    if (!raw_mp) {
        return false;
    }

    mp_floating_pointer_t mp;
    mp_floating_pointer_init(&mp);
    mp_floating_pointer_from_raw(&mp, raw_mp);

    s_mp_table_addr = (uint32_t)(uintptr_t)raw_mp;
    serial_printf("[smp] Found MP Floating Pointer at 0x%x\n", (uint32_t)(uintptr_t)raw_mp);
    if (mp.phys_addr != 0) {
        const uint8_t *cfg = (const uint8_t *)(uintptr_t)mp.phys_addr;
        if (memcmp(cfg, "PCMP", 4) == 0) {
            uint16_t count = *(const uint16_t *)(cfg + 34);
            const uint8_t *entry = cfg + 44;
            uint32_t prev_cpu_count = cpu_count;
            for (uint16_t i = 0; i < count; i++) {
                if (entry[0] == 0 /* Processor */) {
                    uint8_t flags = entry[3];
                    if (flags & 1 /* EN */) {
                        bool is_bsp = (flags & 2) != 0;
                        smp_add_cpu(entry[1], is_bsp);
                    }
                    entry += 20;
                } else {
                    entry += 8;
                }
            }
            if (cpu_count > prev_cpu_count) {
                s_mp_table_enumerated = true;
                s_mp_cpu_count = cpu_count - prev_cpu_count;
                arch_status.mp_table_fallback = SMP_STATUS_ACTIVE;
                mp_floating_pointer_free(&mp);
                return true;
            }
        }
    }

    mp_floating_pointer_free(&mp);
    arch_status.mp_table_fallback = SMP_STATUS_INACTIVE;
    return false;
}

static void probe_cpuid_capabilities(void) {
    if (!check_cpuid_supported()) {
        s_has_cpuid = false;
        s_has_apic = false;
        s_has_htt = false;
        arch_status.cpu_capability_detection = SMP_STATUS_BLOCKED;
        return;
    }

    s_has_cpuid = true;
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);

    char *vbuf = (char *)kmalloc(16);
    if (vbuf) {
        *(uint32_t *)(vbuf + 0) = ebx;
        *(uint32_t *)(vbuf + 4) = edx;
        *(uint32_t *)(vbuf + 8) = ecx;
        vbuf[12] = '\0';
        smp_dyn_str_set(&s_cpu_vendor, &s_cpu_vendor_len, &s_cpu_vendor_cap, vbuf, 12);
        kfree(vbuf);
    }

    if (eax >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        s_has_apic = (edx & (1 << 9)) != 0;
        s_has_htt  = (edx & (1 << 28)) != 0;
    }

    arch_status.cpu_capability_detection = (s_has_cpuid && s_has_apic) ? SMP_STATUS_ACTIVE : SMP_STATUS_BLOCKED;
}

static void detect_via_cpuid(void) {
    probe_cpuid_capabilities();
    if (!s_has_cpuid) {
        serial_printf("[smp] CPUID instruction not supported\n");
        return;
    }

    serial_printf("[smp] CPUID Vendor: %s\n", s_cpu_vendor ? s_cpu_vendor : "Unknown");

    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    if (eax >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        uint32_t max_cores = (ebx >> 16) & 0xFF;

        serial_printf("[smp] CPUID Features: APIC=%s, HTT=%s, Max Logical CPUs=%u\n",
                      s_has_apic ? "yes" : "no", s_has_htt ? "yes" : "no", max_cores);

        if (cpu_count == 0) {
            uint32_t detected = (max_cores > 0) ? max_cores : 1;
            for (uint32_t i = 0; i < detected; i++) {
                smp_add_cpu(i, (i == 0));
            }
        }
    }
}

static void verify_per_cpu_stacks(void) {
    if (cpu_count == 0) {
        s_stacks_allocated = false;
        arch_status.per_cpu_structures = SMP_STATUS_BLOCKED;
        return;
    }
    uint32_t valid = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i].stack_base != 0 && cpus[i].stack_top > cpus[i].stack_base) {
            valid++;
        }
    }
    s_stacks_allocated = (valid == cpu_count);
    arch_status.per_cpu_structures = s_stacks_allocated ? SMP_STATUS_ACTIVE : SMP_STATUS_BLOCKED;
}

static void verify_atomic_spinlocks(void) {
    spinlock_t test_lock = {0};
    s_atomic_locks_verified = false;
    if (spin_trylock(&test_lock)) {
        if (!spin_trylock(&test_lock)) {
            spin_unlock(&test_lock);
            if (test_lock.lock == 0) {
                s_atomic_locks_verified = true;
            }
        } else {
            spin_unlock(&test_lock);
        }
    }
    arch_status.atomic_spinlocks = s_atomic_locks_verified ? SMP_STATUS_ACTIVE : SMP_STATUS_BLOCKED;
}

static void init_per_cpu_stacks(void) {
    for (uint32_t i = 0; i < cpu_count; i++) {
        /* Allocate a 16 KB stack for each CPU */
        void *stack = memory_map_alloc(16384, 4096);
        if (stack) {
            cpus[i].stack_base = (uint32_t)(uintptr_t)stack;
            cpus[i].stack_top = cpus[i].stack_base + 16384;
        } else {
            /* Fallback to static per-cpu offset if early memory exhausted */
            cpus[i].stack_base = 0x500000 + i * 0x10000;
            cpus[i].stack_top = cpus[i].stack_base + 0x10000;
        }
    }
}

static void smp_lapic_enable(void) {
    /* 1. Ensure IA32_APIC_BASE MSR (0x1B) has bit 11 set */
    uint32_t lo, hi;
    cpu_get_msr(IA32_APIC_BASE_MSR, &lo, &hi);
    if (!(lo & APIC_BASE_ENABLE_BIT)) {
        lo |= APIC_BASE_ENABLE_BIT;
        cpu_set_msr(IA32_APIC_BASE_MSR, lo, hi);
    }

    /* 2. Enable Local APIC in SVR (bit 8 = software enable, vector 0xFF) */
    volatile uint32_t *svr = (volatile uint32_t *)(lapic_base_addr + LAPIC_SVR_REG);
    *svr = (*svr & ~0xFFu) | 0x1FFu;

    /* 3. Accept all interrupt priorities */
    volatile uint32_t *tpr = (volatile uint32_t *)(lapic_base_addr + LAPIC_TPR_REG);
    *tpr = 0;
}

static void smp_prepare_trampoline(void) {
    uint32_t len = (uint32_t)(smp_trampoline_end - smp_trampoline_start);
    if (len > 0 && len < 4096) {
        memset((void *)SMP_TRAMPOLINE_ADDR, 0, 256);
        memcpy((void *)SMP_TRAMPOLINE_ADDR, smp_trampoline_start, len);
        serial_printf("[smp] Copied AP trampoline (%u bytes) to physical 0x%08x\n",
                      len, SMP_TRAMPOLINE_ADDR);
    }
}

static spinlock_t s_ipi_lock = {0};

void smp_send_ipi(uint32_t target_lapic_id, uint8_t vector) {
    spin_lock(&s_ipi_lock);
    volatile uint32_t *icr_high = (volatile uint32_t *)(lapic_base_addr + LAPIC_ICR_HIGH);
    volatile uint32_t *icr_low  = (volatile uint32_t *)(lapic_base_addr + LAPIC_ICR_LOW);

    /* Wait for any previous transmission to complete */
    uint32_t timeout = 50000;
    while ((*icr_low & (1u << 12)) && timeout > 0) {
        __builtin_ia32_pause();
        timeout--;
    }

    *icr_high = (target_lapic_id << 24);
    *icr_low  = (0x00004000u | vector);

    timeout = 50000;
    while ((*icr_low & (1u << 12)) && timeout > 0) {
        __builtin_ia32_pause();
        timeout--;
    }
    spin_unlock(&s_ipi_lock);
}

void smp_send_broadcast_ipi(uint8_t vector) {
    spin_lock(&s_ipi_lock);
    volatile uint32_t *icr_low  = (volatile uint32_t *)(lapic_base_addr + LAPIC_ICR_LOW);

    /* Wait for any previous transmission to complete */
    uint32_t timeout = 50000;
    while ((*icr_low & (1u << 12)) && timeout > 0) {
        __builtin_ia32_pause();
        timeout--;
    }

    /* Destination Shorthand: 11b (All excluding self: bits 18=1, 19=1 -> 0xC0000) */
    *icr_low = (0x000C4000u | vector);

    timeout = 50000;
    while ((*icr_low & (1u << 12)) && timeout > 0) {
        __builtin_ia32_pause();
        timeout--;
    }
    spin_unlock(&s_ipi_lock);
}

void smp_ipi_handler(void) {
    uint32_t id = smp_get_current_cpu_id();
    if (ipi_received_count != NULL && id < cpu_count) {
        __atomic_fetch_add(&ipi_received_count[id], 1, __ATOMIC_SEQ_CST);
    }
}

uint32_t smp_get_ipi_count(uint32_t cpu_id) {
    if (ipi_received_count != NULL && cpu_id < cpu_count) return ipi_received_count[cpu_id];
    return 0;
}

/* Cross-core task dispatch */
typedef struct {
    volatile smp_task_fn_t fn;
    void *arg;
    volatile uint8_t *done_flags;
    size_t flags_cap;
    volatile bool active;
} smp_task_desc_t;

static smp_task_desc_t s_broadcast_task = {0};
static spinlock_t s_task_lock = {0};

static void smp_check_and_execute_task(uint32_t cpu_id) {
    if (__atomic_load_n(&s_broadcast_task.active, __ATOMIC_ACQUIRE) && s_broadcast_task.fn) {
        if (s_broadcast_task.done_flags && cpu_id < cpu_count && !s_broadcast_task.done_flags[cpu_id]) {
            s_broadcast_task.fn(cpu_id, s_broadcast_task.arg);
            __atomic_store_n(&s_broadcast_task.done_flags[cpu_id], 1, __ATOMIC_SEQ_CST);
        }
    }
}

void smp_broadcast_task(smp_task_fn_t fn, void *arg) {
    if (!fn) return;
    spin_lock(&s_task_lock);

    if (cpu_count > s_broadcast_task.flags_cap || !s_broadcast_task.done_flags) {
        size_t new_cap = (s_broadcast_task.flags_cap == 0) ? 8 : s_broadcast_task.flags_cap;
        while (new_cap < cpu_count) new_cap *= 2;
        volatile uint8_t *new_f = (volatile uint8_t *)krealloc((void *)s_broadcast_task.done_flags, new_cap);
        if (!new_f) {
            spin_unlock(&s_task_lock);
            return;
        }
        s_broadcast_task.done_flags = new_f;
        s_broadcast_task.flags_cap = new_cap;
    }

    memset((void *)s_broadcast_task.done_flags, 0, cpu_count);
    s_broadcast_task.fn = fn;
    s_broadcast_task.arg = arg;
    __atomic_store_n(&s_broadcast_task.active, true, __ATOMIC_RELEASE);

    /* Execute on BSP */
    fn(0, arg);
    __atomic_store_n(&s_broadcast_task.done_flags[0], 1, __ATOMIC_SEQ_CST);

    /* Wait for all online APs to complete */
    uint32_t timeout = 100000;
    while (timeout > 0) {
        bool all_done = true;
        for (uint32_t i = 0; i < cpu_count; i++) {
            if (cpus[i].is_online && !s_broadcast_task.done_flags[i]) {
                all_done = false;
                break;
            }
        }
        if (all_done) break;
        __builtin_ia32_pause();
        timeout--;
    }

    __atomic_store_n(&s_broadcast_task.active, false, __ATOMIC_RELEASE);
    s_broadcast_task.fn = NULL;
    spin_unlock(&s_task_lock);
}

void smp_test_ipi(void) {
    if (active_cpu_count <= 1) {
        printf("SMP Test: Only 1 active CPU present, skipping cross-core IPI.\n");
        return;
    }
    printf("SMP Test: Broadcasting test IPI to online secondary cores...\n");
    smp_send_broadcast_ipi(SMP_IPI_VECTOR);
}

void ap_main(uint32_t cpu_id) {
    if (cpu_id == 0 || cpu_id >= cpu_count) {
        while (1) { __asm__ volatile("cli; hlt"); }
    }

    /* 1. Load exact BSP IDT on this core */
    __asm__ volatile("lidt %0" : : "m"(s_bsp_idt_ptr));


    /* 2. Enable Local APIC on this core */
    smp_lapic_enable();

    /* Mask LINT0 and LINT1 so legacy 8259 PIC IRQs never hit this AP */
    volatile uint32_t *lint0 = (volatile uint32_t *)(lapic_base_addr + LAPIC_LINT0_REG);
    volatile uint32_t *lint1 = (volatile uint32_t *)(lapic_base_addr + LAPIC_LINT1_REG);
    *lint0 = 0x00010000u;
    *lint1 = 0x00010000u;

    /* 3. Read hardware LAPIC ID and verify against expected APIC ID */
    volatile uint32_t *lapic_id_reg = (volatile uint32_t *)(lapic_base_addr + LAPIC_ID_REG);
    uint32_t hw_lapic_id = (*lapic_id_reg >> 24) & 0xFF;
    if (cpus[cpu_id].lapic_id != hw_lapic_id) {
        while (1) { __asm__ volatile("cli; hlt"); }
    }

    /* 4. Mark online and atomically increment active core counter */
    cpus[cpu_id].is_online = true;
    __atomic_fetch_add(&active_cpu_count, 1, __ATOMIC_SEQ_CST);

    /* 5. Check in with BSP */
    *(volatile uint32_t *)SMP_BOOT_DONE_ADDR = 1;
    __atomic_store_n(&smp_ap_boot_done, 1, __ATOMIC_SEQ_CST);

    serial_printf("[smp] AP core %u online (LAPIC ID=%u, stack=0x%x)\n",
                  cpu_id, hw_lapic_id, cpus[cpu_id].stack_top);

    /* 6. Enable interrupts so this core can receive IPIs */
    __asm__ volatile("sti");

    /* 7. AP worker / diagnostic loop */
    while (smp_running) {
        __atomic_fetch_add(&cpus[cpu_id].ticks, 1, __ATOMIC_RELAXED);

        smp_check_and_execute_task(cpu_id);

        for (volatile int i = 0; i < 50000; i++) {
            __builtin_ia32_pause();
        }
    }

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

static bool smp_boot_ap(uint32_t cpu_id) {
    if (cpu_id >= cpu_count || cpus[cpu_id].is_bsp) return false;

    uint32_t target_lapic = cpus[cpu_id].lapic_id;

    *(volatile uint32_t *)SMP_BOOT_STACK_ADDR = cpus[cpu_id].stack_top;
    *(volatile uint32_t *)SMP_BOOT_CPUID_ADDR = cpu_id;
    *(volatile uint32_t *)SMP_BOOT_FN_ADDR    = (uint32_t)ap_main;
    *(volatile uint32_t *)SMP_BOOT_DONE_ADDR  = 0;
    smp_ap_boot_done = 0;

    serial_printf("[smp] Booting AP core %u (LAPIC ID=%u, stack=0x%x)...\n",
                  cpu_id, target_lapic, cpus[cpu_id].stack_top);

    volatile uint32_t *icr_high = (volatile uint32_t *)(lapic_base_addr + LAPIC_ICR_HIGH);
    volatile uint32_t *icr_low  = (volatile uint32_t *)(lapic_base_addr + LAPIC_ICR_LOW);

    /* 1. Send INIT IPI (Assert, Level, delivery mode 101b = 0x4500) */
    *icr_high = (target_lapic << 24);
    *icr_low  = 0x00004500u;
    smp_delay_us(200);
    uint32_t wait_timeout = 50000;
    while ((*icr_low & (1u << 12)) && wait_timeout > 0) {
        __builtin_ia32_pause();
        wait_timeout--;
    }

    /* 2. Wait 10 ms */
    smp_delay_ms(10);

    /* 3. De-assert INIT IPI (Level, delivery mode 101b = 0x8500) */
    *icr_high = (target_lapic << 24);
    *icr_low  = 0x00008500u;
    smp_delay_us(200);
    wait_timeout = 50000;
    while ((*icr_low & (1u << 12)) && wait_timeout > 0) {
        __builtin_ia32_pause();
        wait_timeout--;
    }

    /* 4. First Startup IPI (vector 0x08 -> physical address 0x8000) */
    *icr_high = (target_lapic << 24);
    *icr_low  = 0x00004608u;
    smp_delay_us(500);
    wait_timeout = 50000;
    while ((*icr_low & (1u << 12)) && wait_timeout > 0) {
        __builtin_ia32_pause();
        wait_timeout--;
    }

    /* Wait up to 1 ms */
    for (int t = 0; t < 100; t++) {
        if (smp_ap_boot_done || *(volatile uint32_t *)SMP_BOOT_DONE_ADDR) break;
        smp_delay_us(10);
    }

    /* 5. Second Startup IPI if needed */
    if (!smp_ap_boot_done && !(*(volatile uint32_t *)SMP_BOOT_DONE_ADDR)) {
        *icr_high = (target_lapic << 24);
        *icr_low  = 0x00004608u;
        smp_delay_us(500);
        wait_timeout = 50000;
        while ((*icr_low & (1u << 12)) && wait_timeout > 0) {
            __builtin_ia32_pause();
            wait_timeout--;
        }

        for (int t = 0; t < 5000; t++) {
            if (smp_ap_boot_done || *(volatile uint32_t *)SMP_BOOT_DONE_ADDR) break;
            smp_delay_us(10);
        }
    }

    if (smp_ap_boot_done || *(volatile uint32_t *)SMP_BOOT_DONE_ADDR) {
        cpus[cpu_id].is_online = true;
        serial_printf("[smp] AP core %u successfully booted and verified ONLINE\n", cpu_id);
        return true;
    } else {
        serial_printf("[smp] AP core %u timed out during INIT-SIPI-SIPI sequence\n", cpu_id);
        return false;
    }
}

void smp_init(void) {
    cpu_count = 0;
    active_cpu_count = 1; /* BSP always active */

    /* 1. Dynamically probe CPU capabilities & atomic primitives */
    probe_cpuid_capabilities();
    verify_atomic_spinlocks();

    /* 2. Try ACPI MADT enumeration */
    if (!parse_acpi_madt()) {
        arch_status.acpi_madt_enumeration = SMP_STATUS_INACTIVE;
        /* Fallback to MP Table */
        if (!parse_mp_table()) {
            arch_status.mp_table_fallback = SMP_STATUS_INACTIVE;
            /* Fallback to CPUID */
            detect_via_cpuid();
        }
    } else {
        arch_status.mp_table_fallback = SMP_STATUS_INACTIVE;
    }

    if (cpu_count == 0) {
        /* Default single-core fallback */
        smp_add_cpu(0, true);
    }

    /* 3. Initialize & dynamically verify per-CPU stacks */
    init_per_cpu_stacks();
    verify_per_cpu_stacks();

    /* 4. Enable Local APIC on BSP and capture BSP IDTR */
    smp_lapic_enable();
    __asm__ volatile("sidt %0" : "=m"(s_bsp_idt_ptr));

    /* 5. Prepare AP trampoline at 0x8000 */
    smp_prepare_trampoline();

    /* 6. Boot AP cores (cores 1..cpu_count-1) */
    for (uint32_t i = 1; i < cpu_count; i++) {
        smp_boot_ap(i);
    }

    /* Recount active CPUs */
    active_cpu_count = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i].is_online) active_cpu_count++;
    }

    /* Dynamically evaluate AP boot sequence */
    if (cpu_count <= 1) {
        arch_status.ap_boot_sequence = SMP_STATUS_INACTIVE;
    } else if (active_cpu_count > 1) {
        arch_status.ap_boot_sequence = SMP_STATUS_ACTIVE;
    } else {
        arch_status.ap_boot_sequence = SMP_STATUS_BLOCKED;
    }

    /* Dynamically evaluate multi-core subsystems based on active core count */
    if (active_cpu_count <= 1) {
        arch_status.driver_reentrancy = SMP_STATUS_INACTIVE;
        arch_status.ipi_cross_core_reschedule = SMP_STATUS_INACTIVE;
        arch_status.cross_core_scheduler = SMP_STATUS_INACTIVE;
    } else {
        arch_status.driver_reentrancy = SMP_STATUS_BLOCKED;
        arch_status.ipi_cross_core_reschedule = SMP_STATUS_BLOCKED;
        arch_status.cross_core_scheduler = SMP_STATUS_BLOCKED;
    }

    serial_printf("[smp] SMP Initialization Complete: %u core(s) detected, %u core(s) active\n",
                  cpu_count, active_cpu_count);

    smp_print_summary();
}

uint32_t smp_get_cpu_count(void) {
    return cpu_count;
}

uint32_t smp_get_active_cpu_count(void) {
    return active_cpu_count;
}

const cpu_info_t *smp_get_cpu_info(uint32_t cpu_id) {
    if (cpu_id >= cpu_count) return NULL;
    return &cpus[cpu_id];
}

uint32_t smp_get_current_cpu_id(void) {
    if (lapic_base_addr) {
        volatile uint32_t *lapic_id_reg = (volatile uint32_t *)(lapic_base_addr + LAPIC_ID_REG);
        uint32_t hw_id = (*lapic_id_reg >> 24) & 0xFF;
        for (uint32_t i = 0; i < cpu_count; i++) {
            if (cpus[i].lapic_id == hw_id) return cpus[i].cpu_id;
        }
    }
    return 0;
}

const smp_architecture_status_t *smp_get_architecture_status(void) {
    return &arch_status;
}

uint32_t smp_get_lapic_base(void) {
    return lapic_base_addr;
}

void smp_print_summary(void) {
    serial_printf("=== SMP Architectural & Capability Report ===\n");
    serial_printf("  Detected CPUs:             %u\n", cpu_count);
    serial_printf("  Active CPUs:               %u\n", active_cpu_count);
    serial_printf("  LAPIC Base:                0x%x\n", lapic_base_addr);
    serial_printf("  Subsystem Status:\n");

    /* CPU Capability */
    if (arch_status.cpu_capability_detection == SMP_STATUS_ACTIVE) {
        serial_printf("    CPU Capability (CPUID):  ACTIVE (Vendor: %s, APIC: %s, HTT: %s)\n",
                      s_cpu_vendor ? s_cpu_vendor : "Unknown", s_has_apic ? "yes" : "no", s_has_htt ? "yes" : "no");
    } else {
        serial_printf("    CPU Capability (CPUID):  BLOCKED (CPUID or APIC unsupported)\n");
    }

    /* ACPI MADT */
    if (arch_status.acpi_madt_enumeration == SMP_STATUS_ACTIVE) {
        serial_printf("    ACPI MADT Enumeration:   ACTIVE (%u CPU(s) discovered at 0x%x)\n",
                      s_madt_cpu_count, s_madt_addr);
    } else {
        serial_printf("    ACPI MADT Enumeration:   INACTIVE (ACPI MADT table not found)\n");
    }

    /* MP Table Fallback */
    if (arch_status.mp_table_fallback == SMP_STATUS_ACTIVE) {
        serial_printf("    MP Table Fallback:       ACTIVE (%u CPU(s) discovered at 0x%x)\n",
                      s_mp_cpu_count, s_mp_table_addr);
    } else if (s_madt_enumerated) {
        serial_printf("    MP Table Fallback:       INACTIVE (Bypassed: ACPI MADT succeeded)\n");
    } else {
        serial_printf("    MP Table Fallback:       INACTIVE (MP floating pointer not found)\n");
    }

    /* Per-CPU Stacks & State */
    if (arch_status.per_cpu_structures == SMP_STATUS_ACTIVE) {
        serial_printf("    Per-CPU Stacks & State:  ACTIVE (%u stack(s) allocated, 16KB/core)\n", cpu_count);
    } else {
        serial_printf("    Per-CPU Stacks & State:  BLOCKED (Stack allocation failure)\n");
    }

    /* Atomic Bus Spinlocks */
    if (arch_status.atomic_spinlocks == SMP_STATUS_ACTIVE) {
        serial_printf("    Atomic Bus Spinlocks:    ACTIVE (Hardware test-and-set verified)\n");
    } else {
        serial_printf("    Atomic Bus Spinlocks:    BLOCKED (Atomic lock self-test failed)\n");
    }

    /* AP Boot Sequence */
    if (arch_status.ap_boot_sequence == SMP_STATUS_ACTIVE) {
        serial_printf("    AP Boot Sequence (ICR):  ACTIVE (Trampoline at 0x%x, %u/%u core(s) booted)\n",
                      SMP_TRAMPOLINE_ADDR, active_cpu_count, cpu_count);
    } else if (arch_status.ap_boot_sequence == SMP_STATUS_INACTIVE) {
        serial_printf("    AP Boot Sequence (ICR):  INACTIVE (Uniprocessor system, no AP cores to boot)\n");
    } else {
        serial_printf("    AP Boot Sequence (ICR):  BLOCKED (INIT-SIPI-SIPI timeout, 0/%u AP cores booted)\n",
                      cpu_count > 1 ? cpu_count - 1 : 0);
    }

    /* Driver Reentrancy */
    if (arch_status.driver_reentrancy == SMP_STATUS_ACTIVE) {
        serial_printf("    Driver Reentrancy:       ACTIVE\n");
    } else if (arch_status.driver_reentrancy == SMP_STATUS_INACTIVE) {
        serial_printf("    Driver Reentrancy:       INACTIVE (Single-core mode, concurrent driver access not present)\n");
    } else {
        serial_printf("    Driver Reentrancy:       BLOCKED (ATA/VGA/Net drivers use static non-reentrant state)\n");
    }

    /* IPI Cross-Core Reschedule */
    if (arch_status.ipi_cross_core_reschedule == SMP_STATUS_ACTIVE) {
        serial_printf("    IPI Cross-Core Preempt:  ACTIVE (Vector 0x%x registered)\n", SMP_IPI_VECTOR);
    } else if (arch_status.ipi_cross_core_reschedule == SMP_STATUS_INACTIVE) {
        serial_printf("    IPI Cross-Core Preempt:  INACTIVE (Single-core mode, cross-core IPI not needed)\n");
    } else {
        serial_printf("    IPI Cross-Core Preempt:  BLOCKED (Requires per-CPU APIC timer preemptive vector)\n");
    }

    /* Multi-Queue SMP Scheduler */
    if (arch_status.cross_core_scheduler == SMP_STATUS_ACTIVE) {
        serial_printf("    Multi-Queue SMP Sched:   ACTIVE\n");
    } else if (arch_status.cross_core_scheduler == SMP_STATUS_INACTIVE) {
        serial_printf("    Multi-Queue SMP Sched:   INACTIVE (Single-core mode, cooperative scheduler active)\n");
    } else {
        serial_printf("    Multi-Queue SMP Sched:   BLOCKED (Requires non-blocking multi-core runqueue)\n");
    }

    serial_printf("=============================================\n");
}

void smp_get_cpu_vendor(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return;
    buf[0] = '\0';
    if (!check_cpuid_supported()) {
        strncpy(buf, "Generic x86", max_len - 1);
        buf[max_len - 1] = '\0';
        return;
    }
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    char *vendor = (char *)kmalloc(16);
    if (!vendor) return;
    *(uint32_t *)(vendor + 0) = ebx;
    *(uint32_t *)(vendor + 4) = edx;
    *(uint32_t *)(vendor + 8) = ecx;
    vendor[12] = '\0';
    strncpy(buf, vendor, max_len - 1);
    buf[max_len - 1] = '\0';
    kfree(vendor);
}

void smp_get_cpu_brand(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return;
    buf[0] = '\0';
    if (!check_cpuid_supported()) {
        strncpy(buf, "Processor", max_len - 1);
        buf[max_len - 1] = '\0';
        return;
    }

    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000000u, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004u) {
        size_t brand_sz = 64;
        char *brand = (char *)kmalloc(brand_sz);
        if (brand) {
            memset(brand, 0, brand_sz);
            cpuid(0x80000002u, (uint32_t *)(brand + 0),  (uint32_t *)(brand + 4),  (uint32_t *)(brand + 8),  (uint32_t *)(brand + 12));
            cpuid(0x80000003u, (uint32_t *)(brand + 16), (uint32_t *)(brand + 20), (uint32_t *)(brand + 24), (uint32_t *)(brand + 28));
            cpuid(0x80000004u, (uint32_t *)(brand + 32), (uint32_t *)(brand + 36), (uint32_t *)(brand + 40), (uint32_t *)(brand + 44));
            brand[48] = '\0';

            /* Trim leading whitespace from CPU brand string */
            const char *p = brand;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '\0') {
                strncpy(buf, p, max_len - 1);
                buf[max_len - 1] = '\0';
                kfree(brand);
                return;
            }
            kfree(brand);
        }
    }

    /* Fallback if extended brand string leaf is not present */
    size_t vlen = 64;
    char *vendor = (char *)kmalloc(vlen);
    if (vendor) {
        smp_get_cpu_vendor(vendor, vlen);
        uint32_t family = 0, model = 0, stepping = 0;
        smp_get_cpu_model_info(&family, &model, &stepping);
        snprintf(buf, max_len, "%s Family %u Model %u Stepping %u", vendor, family, model, stepping);
        kfree(vendor);
    }
}

void smp_get_cpu_model_info(uint32_t *family, uint32_t *model, uint32_t *stepping) {
    if (family) *family = 0;
    if (model) *model = 0;
    if (stepping) *stepping = 0;
    if (!check_cpuid_supported()) return;

    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    if (eax >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        uint32_t step = eax & 0x0F;
        uint32_t mod  = (eax >> 4) & 0x0F;
        uint32_t fam  = (eax >> 8) & 0x0F;
        uint32_t ext_mod = (eax >> 16) & 0x0F;
        uint32_t ext_fam = (eax >> 20) & 0xFF;

        if (fam == 6 || fam == 15) {
            mod += (ext_mod << 4);
        }
        if (fam == 15) {
            fam += ext_fam;
        }

        if (stepping) *stepping = step;
        if (model)    *model = mod;
        if (family)   *family = fam;
    }
}

void smp_get_cpu_features_str(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return;
    buf[0] = '\0';
    if (!check_cpuid_supported()) {
        strncpy(buf, "None", max_len - 1);
        buf[max_len - 1] = '\0';
        return;
    }

    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    if (eax < 1) {
        strncpy(buf, "CPUID v0", max_len - 1);
        buf[max_len - 1] = '\0';
        return;
    }

    cpuid(1, &eax, &ebx, &ecx, &edx);

    static const struct {
        bool in_ecx;
        uint32_t bit;
        const char *name;
    } feature_list[] = {
        {false, 0,  "FPU"},    {false, 1,  "VME"},    {false, 2,  "DE"},
        {false, 3,  "PSE"},    {false, 4,  "TSC"},    {false, 5,  "MSR"},
        {false, 6,  "PAE"},    {false, 7,  "MCE"},    {false, 8,  "CX8"},
        {false, 9,  "APIC"},   {false, 11, "SEP"},    {false, 12, "MTRR"},
        {false, 13, "PGE"},    {false, 14, "MCA"},    {false, 15, "CMOV"},
        {false, 16, "PAT"},    {false, 17, "PSE36"},  {false, 19, "CLFSH"},
        {false, 23, "MMX"},    {false, 24, "FXSR"},   {false, 25, "SSE"},
        {false, 26, "SSE2"},   {false, 28, "HTT"},
        {true,  0,  "SSE3"},   {true,  1,  "PCLMUL"}, {true,  3,  "MONITOR"},
        {true,  9,  "SSSE3"},  {true,  12, "FMA"},    {true,  13, "CX16"},
        {true,  19, "SSE4.1"}, {true,  20, "SSE4.2"}, {true,  21, "x2APIC"},
        {true,  22, "MOVBE"},  {true,  23, "POPCNT"}, {true,  25, "AES"},
        {true,  26, "XSAVE"},  {true,  28, "AVX"},    {true,  30, "RDRAND"}
    };

    size_t pos = 0;
    bool first = true;
    for (size_t i = 0; i < sizeof(feature_list) / sizeof(feature_list[0]); i++) {
        uint32_t reg = feature_list[i].in_ecx ? ecx : edx;
        if (reg & (1u << feature_list[i].bit)) {
            if (!first && pos < max_len - 2) {
                buf[pos++] = ' ';
            }
            first = false;
            const char *fn = feature_list[i].name;
            while (*fn && pos < max_len - 1) {
                buf[pos++] = *fn++;
            }
            buf[pos] = '\0';
        }
    }
}
