#include <system/pit.h>
#include <ioport.h>

uint16_t pit_get_divisor(uint32_t hz) {
    return PIT_FREQUENCY / hz;
}

void pit_set_frequency(uint8_t counter, uint32_t hz) {
    if (hz == 0 || hz > PIT_FREQUENCY)
        return;
    
    uint16_t divisor = pit_get_divisor(hz);
    
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
