#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* Kernel configuration of the nornStore host tests: a task that uses the store while a
   second one reads it, and the mutex between them. */

#define RTOS_CFG_NUM_TASKS      2
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_EVT_BITS       16
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#define RTOS_CFG_IDLE_HOOK      test_idleHook

void test_idleHook(void);

#endif  /* RTOS_CONFIG_INCLUDED */
