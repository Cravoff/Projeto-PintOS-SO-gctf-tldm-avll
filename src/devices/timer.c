#include "devices/timer.h"
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

/* (ALARM CLOCK) MODIFICAÇÃO: CRIAÇÃO DA LISTA DE ESPERA DE THREADS BLOQUEADAS
 * POR TEMPO */
static struct list blocked_list;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);
static void real_time_delay(int64_t num, int32_t denom);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void timer_init(void) {
  pit_configure_channel(0, 2, TIMER_FREQ);
  intr_register_ext(0x20, timer_interrupt, "8254 Timer");
  list_init(&blocked_list); /* (ALARM CLOCK) MODIFICAÇÃO: INICIALIZAÇÃO A LISTA
                               DE THREADS BLOQUEADAS */
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void timer_calibrate(void) {
  unsigned high_bit, test_bit;

  ASSERT(intr_get_level() == INTR_ON);
  printf("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops(loops_per_tick << 1)) {
    loops_per_tick <<= 1;
    ASSERT(loops_per_tick != 0);
  }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops(loops_per_tick | test_bit))
      loops_per_tick |= test_bit;

  printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t timer_ticks(void) {
  enum intr_level old_level = intr_disable();
  int64_t t = ticks;
  intr_set_level(old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t timer_elapsed(int64_t then) { return timer_ticks() - then; }

/* (ALARM CLOCK) MODIFICAÇÃO: FUNÇÃO AUXILIAR PARA INSERÇÃO EM ORDEM NA LISTA DE
THREADS BLOQUEADAS */
bool check_wakeup_time(const struct list_elem *a, const struct list_elem *b,
                       void *aux UNUSED) {
  struct thread *thread_a = list_entry(a, struct thread, elem);
  struct thread *thread_b = list_entry(b, struct thread, elem);
  return thread_a->wakeup_time < thread_b->wakeup_time;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void timer_sleep(int64_t ticks) {
  int64_t start = timer_ticks();

  ASSERT(intr_get_level() == INTR_ON);

  /*
    while (timer_elapsed (start) < ticks)
      thread_yield ();
  */

  /* (ALARM CLOCK) MODIFICAÇÃO: DESABILITA AS INTERRUPÇÕES PARA BLOQUEAR E
  ADICIONAR A THREAD NA LISTA DE THREADS BLOQUEADAS (DE MANEIRA ORDENADA, OU
  SEJA, A THREAD COM MENOR TEMPO DE ESPERA ESTÁ NO INÍCIO DA LISTA) */
  struct thread *cur = thread_current();
  cur->wakeup_time = start + ticks;

  enum intr_level prev_level = intr_disable();

  list_insert_ordered(&blocked_list, &cur->elem,
                      (list_less_func *)check_wakeup_time, NULL);
  thread_block();

  intr_set_level(prev_level);
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void timer_msleep(int64_t ms) { real_time_sleep(ms, 1000); }

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void timer_usleep(int64_t us) { real_time_sleep(us, 1000 * 1000); }

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void timer_nsleep(int64_t ns) { real_time_sleep(ns, 1000 * 1000 * 1000); }

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void timer_mdelay(int64_t ms) { real_time_delay(ms, 1000); }

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void timer_udelay(int64_t us) { real_time_delay(us, 1000 * 1000); }

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void timer_ndelay(int64_t ns) { real_time_delay(ns, 1000 * 1000 * 1000); }

/* Prints timer statistics. */
void timer_print_stats(void) {
  printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

/* Timer interrupt handler. */
static void timer_interrupt(struct intr_frame *args UNUSED) {
  ticks++;
  thread_tick ();

  /* (ALARM CLOCK) MODIFICAÇÃO: SE A LISTA NÃO ESTIVER VAZIA E O TEMPO PARA
  ACORDAR DA PRIMEIRA THREAD DA LISTA FOR ULTRAPASSADO, DESBLOQUEIE TODAS AS
  THREADS CUJO TEMPO DE ESPERA JÁ PASSOU */

  while (
      !list_empty(&blocked_list) &&
      list_entry(list_front(&blocked_list), struct thread, elem)->wakeup_time <=
          ticks) {
    struct list_elem *cur = list_pop_front(&blocked_list);
    struct thread *t = list_entry(cur, struct thread, elem);
    thread_unblock(t);

    // CAUSA UMA TROCA DE CONTEXTO CASO A THREAD ACORDADA TENHA PRIORIDADE MAIOR
    // QUE A RODANDO ATUALMENTE

    if (t->priority > thread_current()->priority) {
      intr_yield_on_return();
    }
  }

  if (thread_mlfqs) {
    /* 1) Incrementa recent_cpu da thread atual a cada tick */
    struct thread *cur = thread_current();
    if (cur != idle_thread)
        cur->recent_cpu = fp_add_int(cur->recent_cpu, 1);

    /* 2) A cada 4 ticks: recalcula prioridade da thread atual */
    if (ticks % 4 == 0)
        mlfqs_update_priority(thread_current());

    /* 3) A cada segundo: recalcula load_avg e recent_cpu de todos */
    if (ticks % TIMER_FREQ == 0)
        mlfqs_update_all();
  }
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool too_many_loops(unsigned loops) {
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait(loops);

  /* If the tick count changed, we iterated too long. */
  barrier();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE busy_wait(int64_t loops) {
  while (loops-- > 0)
    barrier();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void real_time_sleep(int64_t num, int32_t denom) {
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.

        (NUM / DENOM) s
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT(intr_get_level() == INTR_ON);
  if (ticks > 0) {
    /* We're waiting for at least one full timer tick.  Use
       timer_sleep() because it will yield the CPU to other
       processes. */
    timer_sleep(ticks);
  } else {
    /* Otherwise, use a busy-wait loop for more accurate
       sub-tick timing. */
    real_time_delay(num, denom);
  }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void real_time_delay(int64_t num, int32_t denom) {
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT(denom % 1000 == 0);
  busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
}
