#include <system/timer.h>
#include <system/pit.h>
#include <kernel/terminal.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <driver/audio_core.h>
#include <stdio.h>
#include <ioport.h>

#define PIT_CALIB_PORT61     0x61
#define PIT_CALIB_MS         10u
#define PIT_CALIB_TICKS      ((PIT_FREQUENCY * PIT_CALIB_MS) / 1000u)
#define PIT_CALIB_TRIALS     3u
#define TSC_FALLBACK_PER_MS  1000000ULL

static volatile uint32_t timer_ms = 0;
static volatile uint32_t timer_ticks = 0;
static uint64_t tsc_per_ms = TSC_FALLBACK_PER_MS;
static uint64_t last_tsc_value = 0ULL;
static uint32_t last_slice_yield_ms = 0;

uint64_t read_tsc(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void cpuid_query(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

static uint64_t calibrate_tsc_per_ms(void) {
    uint32_t max_leaf = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid_query(0, &max_leaf, &ebx, &ecx, &edx);

    /* 1. Try CPUID Leaf 0x15 (TSC / Core Crystal Clock ratio) */
    if (max_leaf >= 0x15u) {
        uint32_t denom = 0, numer = 0, crystal_hz = 0, res = 0;
        cpuid_query(0x15u, &denom, &numer, &crystal_hz, &res);
        if (denom != 0u && numer != 0u && crystal_hz != 0u) {
            uint64_t tsc_hz = ((uint64_t)crystal_hz * (uint64_t)numer) / (uint64_t)denom;
            if (tsc_hz >= 1000u) {
                return tsc_hz / 1000ULL;
            }
        }
    }

    /* 2. Try CPUID Leaf 0x16 (Processor Base Frequency in MHz) */
    if (max_leaf >= 0x16u) {
        uint32_t base_mhz = 0, max_mhz = 0, bus_mhz = 0, res = 0;
        cpuid_query(0x16u, &base_mhz, &max_mhz, &bus_mhz, &res);
        base_mhz &= 0xFFFFu;
        if (base_mhz != 0u) {
            return (uint64_t)base_mhz * 1000ULL;
        }
    }

    /* Calibrate against 8254 PIT Channel 2 using the median of 3 gates */
    uint32_t irq_flags = irq_save();

    uint8_t orig_gate = inb(PIT_CALIB_PORT61);
    uint64_t m0 = 0ULL, m1 = 0ULL, m2 = 0ULL;

    for (uint32_t trial = 0; trial < PIT_CALIB_TRIALS; trial++) {
        /* Keep GATE2 low and speaker disabled while programming Mode 0 count */
        outb(PIT_CALIB_PORT61, (uint8_t)(orig_gate & ~0x03u));
        io_wait();

        /* Channel 2, lobyte/hibyte, Mode 0 (Interrupt on Terminal Count), binary */
        outb(PIT_REG_COMMAND, PIT_SELECT_COUNTER_2 | PIT_WRITE_LSB_MSB | PIT_BINARY_MODE);
        io_wait();
        outb(PIT_REG_COUNTER_2, (uint8_t)(PIT_CALIB_TICKS & 0xFFu));
        io_wait();
        outb(PIT_REG_COUNTER_2, (uint8_t)((PIT_CALIB_TICKS >> 8) & 0xFFu));
        io_wait();

        /* Assert GATE2 high to start counting immediately */
        outb(PIT_CALIB_PORT61, (uint8_t)((orig_gate & ~0x02u) | 0x01u));
        uint64_t start_tsc = read_tsc();
        uint32_t max_polls = 20000000u;
        while ((inb(PIT_CALIB_PORT61) & 0x20u) == 0u && --max_polls > 0u) {
            __asm__ volatile("pause");
        }
        uint64_t end_tsc = read_tsc();

        uint64_t sample_ms = (end_tsc - start_tsc) / (uint64_t)PIT_CALIB_MS;
        if (trial == 0u) m0 = sample_ms;
        else if (trial == 1u) m1 = sample_ms;
        else m2 = sample_ms;
    }

    outb(PIT_CALIB_PORT61, orig_gate);
    irq_restore(irq_flags);

    if (m0 > m1) { uint64_t tmp = m0; m0 = m1; m1 = tmp; }
    if (m1 > m2) { uint64_t tmp = m1; m1 = m2; m2 = tmp; }
    if (m0 > m1) { uint64_t tmp = m0; m0 = m1; m1 = tmp; }

    uint64_t measured_per_ms = (m1 != 0ULL) ? m1 : ((m2 != 0ULL) ? m2 : m0);
    if (measured_per_ms == 0ULL) {
        measured_per_ms = TSC_FALLBACK_PER_MS;
    }
    return measured_per_ms;
}

uint64_t timer_get_tsc_per_ms(void) {
    return tsc_per_ms;
}

void timer_init(void) {
    timer_ms = 0;
    timer_ticks = 0;
    tsc_per_ms = calibrate_tsc_per_ms();
    last_tsc_value = read_tsc();
    last_slice_yield_ms = 0;
    pit_init(1000u);
    printf("[timer] PIT initialized at 1000 Hz, calibrated TSC: %u MHz (%u ticks/ms).\n",
           (uint32_t)(tsc_per_ms / 1000ULL), (uint32_t)tsc_per_ms);
}

void timer_update(void) {
    uint32_t irq_flags = irq_save();
    uint64_t now_tsc = read_tsc();
    if (last_tsc_value == 0ULL) {
        last_tsc_value = now_tsc;
        irq_restore(irq_flags);
        return;
    }
    if (now_tsc <= last_tsc_value) {
        irq_restore(irq_flags);
        return;
    }

    uint64_t delta_tsc = now_tsc - last_tsc_value;
    uint32_t delta_ms = (uint32_t)(delta_tsc / tsc_per_ms);
    if (delta_ms != 0u) {
        timer_ms += delta_ms;
        timer_ticks += delta_ms;
        last_tsc_value += (uint64_t)delta_ms * tsc_per_ms;
    }
    irq_restore(irq_flags);
}

static void timer_dispatch_tick(void) {
    audio_poll();
    if (process_is_batch_active() && process_in_process_context() && !putchar_is_locked()) {
        if (timer_ms - last_slice_yield_ms >= TIMER_SLICE_YIELD_MS) {
            last_slice_yield_ms = timer_ms;
            process_yield_kernel();
        }
    }
}

void timer_tick(void) {
    timer_update();
    timer_dispatch_tick();
}

void timer_irq_handler(void) {
    uint32_t irq_flags = irq_save();
    uint64_t before_tsc = last_tsc_value;
    uint32_t prev_ms = timer_ms;
    irq_restore(irq_flags);

    timer_update();

    irq_flags = irq_save();
    if (timer_ms == prev_ms && read_tsc() == before_tsc) {
        /* Fallback only if hardware TSC is not advancing */
        timer_ms++;
        timer_ticks++;
    }
    irq_restore(irq_flags);

    timer_dispatch_tick();
}

uint32_t timer_millis(void) {
    timer_update();
    return timer_ms;
}

uint32_t timer_seconds(void) {
    return timer_millis() / 1000u;
}

uint32_t timer_elapsed_ms(uint32_t start_ms) {
    return timer_millis() - start_ms;
}

int timer_after(uint32_t start_ms, uint32_t delay_ms) {
    return (int)(timer_millis() - start_ms >= delay_ms);
}
