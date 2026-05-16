#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H

#include <list.h>
#include <round.h>
#include <stdint.h>

/* Number of timer interrupts per second. */
#define TIMER_FREQ 100

void timer_init(void);
void timer_calibrate(void);

int64_t timer_ticks(void);
int64_t timer_elapsed(int64_t);

/* (ALARM CLOCK) MODIFICAÇÃO: FUNÇÃO AUXILIAR PARA INSERÇÃO ORDENADA NA LISTA DE
 * THREADS BLOQUEADAS */
bool check_wakeup_time(const struct list_elem *a, const struct list_elem *b,
                       void *aux);

/* Sleep and yield the CPU to other threads. */
void timer_sleep(int64_t ticks);
void timer_msleep(int64_t milliseconds);
void timer_usleep(int64_t microseconds);
void timer_nsleep(int64_t nanoseconds);

/* Busy waits. */
void timer_mdelay(int64_t milliseconds);
void timer_udelay(int64_t microseconds);
void timer_ndelay(int64_t nanoseconds);

void timer_print_stats(void);

#endif /* devices/timer.h */
