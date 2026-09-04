#ifndef _TIMER_H
#define _TIMER_H

#include <stdint.h>

void timer_init(void);
void timer_tick(void);
uint32_t timer_millis(void);
uint32_t timer_seconds(void);
uint32_t timer_elapsed_ms(uint32_t start_ms);
int timer_after(uint32_t start_ms, uint32_t delay_ms);
uint64_t read_tsc(void);

#endif
