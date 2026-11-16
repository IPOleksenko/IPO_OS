#ifndef _PIT_H
#define _PIT_H

#include <stdint.h>

/* PIT I/O ports */
#define PIT_REG_COUNTER_0   0x40
#define PIT_REG_COUNTER_1   0x41
#define PIT_REG_COUNTER_2   0x42  /* PC speaker */
#define PIT_REG_COMMAND     0x43

#define PIT_FREQUENCY 1193182

#define PIT_BINARY_MODE        0x00
#define PIT_BCD_MODE           0x01
#define PIT_SQUARE_WAVE_MODE   0x06  /* Mode 3 */
#define PIT_WRITE_LSB_MSB      0x30  /* 16-bit load: LSB then MSB */

#define PIT_SELECT_COUNTER_0   0x00
#define PIT_SELECT_COUNTER_1   0x40
#define PIT_SELECT_COUNTER_2   0x80

/* Program counter 0 to hz (typically 100 Hz for system tick) */
void pit_init(uint32_t hz);

void pit_set_frequency(uint8_t counter, uint32_t hz);

uint16_t pit_get_divisor(uint32_t hz);

/* Current frequency for counter, or 0 if unprogrammed */
uint32_t pit_get_frequency(uint8_t counter);

#endif
