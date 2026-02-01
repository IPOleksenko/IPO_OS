#ifndef _PIT_H
#define _PIT_H

#include <stdint.h>

/* PIT (Programmable Interval Timer) I/O ports */
#define PIT_REG_COUNTER_0   0x40  /* Timer 0 counter port */
#define PIT_REG_COUNTER_1   0x41  /* Timer 1 counter port */
#define PIT_REG_COUNTER_2   0x42  /* Timer 2 counter port (PC Speaker) */
#define PIT_REG_COMMAND     0x43  /* Control port */

/* PIT frequency in Hz */
#define PIT_FREQUENCY 1193182

/* Control word bits */
#define PIT_BINARY_MODE        0x00  /* Binary mode */
#define PIT_BCD_MODE           0x01  /* BCD mode */
#define PIT_SQUARE_WAVE_MODE   0x06  /* Mode 3: Square wave generator */
#define PIT_WRITE_LSB_MSB      0x30  /* Write 16-bit counter (LSB first, then MSB) */

/* Counter selection */
#define PIT_SELECT_COUNTER_0   0x00  /* Select counter 0 */
#define PIT_SELECT_COUNTER_1   0x40  /* Select counter 1 */
#define PIT_SELECT_COUNTER_2   0x80  /* Select counter 2 */

/**
 * Initialize PIT with specified frequency on counter 0
 * @param hz Frequency in Hz (typically 100 for system timer)
 */
void pit_init(uint32_t hz);

/**
 * Set frequency for PIT counter
 * @param counter Counter number (0, 1, or 2)
 * @param hz Frequency in Hz
 */
void pit_set_frequency(uint8_t counter, uint32_t hz);

/**
 * Get current PIT divisor value
 * @param hz Frequency in Hz
 * @return Divisor value for PIT
 */
uint16_t pit_get_divisor(uint32_t hz);

#endif
