#include <system/pit.h>
#include <ioport.h>

uint16_t pit_get_divisor(uint32_t hz) {
    if (hz == 0) return 0;
    if (hz >= PIT_FREQUENCY) return 1;
    uint32_t d = PIT_FREQUENCY / hz;
    if (d > 65535) d = 65535;
    if (d == 0) d = 1;
    return (uint16_t)d;
}

static uint32_t s_pit0_hz = 0;
static uint32_t s_pit1_hz = 0;
static uint32_t s_pit2_hz = 0;

uint32_t pit_get_frequency(uint8_t counter) {
    switch (counter) {
        case 0: return s_pit0_hz;
        case 1: return s_pit1_hz;
        case 2: return s_pit2_hz;
        default: return 0;
    }
}

void pit_set_frequency(uint8_t counter, uint32_t hz) {
    if (hz == 0)
        return;
    
    uint16_t divisor = pit_get_divisor(hz);
    uint32_t programmed = PIT_FREQUENCY / divisor;
    if (counter == 0) s_pit0_hz = programmed;
    else if (counter == 1) s_pit1_hz = programmed;
    else if (counter == 2) s_pit2_hz = programmed;
    
    /* Select counter and set up control word */
    uint8_t counter_select = 0;
    switch (counter) {
        case 0: counter_select = PIT_SELECT_COUNTER_0; break;
        case 1: counter_select = PIT_SELECT_COUNTER_1; break;
        case 2: counter_select = PIT_SELECT_COUNTER_2; break;
        default: return;
    }
    
    uint8_t ocw = counter_select | PIT_WRITE_LSB_MSB | PIT_BINARY_MODE | PIT_SQUARE_WAVE_MODE;
    outb(PIT_REG_COMMAND, ocw);
    
    /* Write divisor (LSB first, then MSB) */
    outb(PIT_REG_COUNTER_0 + counter, (uint8_t)(divisor & 0xFF));
    io_wait();
    outb(PIT_REG_COUNTER_0 + counter, (uint8_t)((divisor >> 8) & 0xFF));
    io_wait();
}

void pit_init(uint32_t hz) {
    pit_set_frequency(0, hz);
}
