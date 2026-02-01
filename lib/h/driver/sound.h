#ifndef _SOUND_H
#define _SOUND_H

#include <stdint.h>

/**
 * Initialize sound driver
 */
void sound_init(void);

/**
 * Play sound at specified frequency (Hz)
 * @param frequency Frequency in Hz (440 = A4, typical range 37-20000 Hz)
 */
void sound_play(uint16_t frequency);

/**
 * Stop playing sound
 */
void sound_stop(void);

/**
 * Play beep with specified duration
 * @param frequency Frequency in Hz
 * @param duration Duration in milliseconds (approximate)
 */
void sound_beep(uint16_t frequency, uint16_t duration);

#endif
