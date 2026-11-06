/* Exercise the production wait with a completion at a preemption boundary.
 * Unused supervisor functions are discarded at link time. */
#include <stdio.h>
#ifdef TEST_MANAGED_SERVICE_WAIT
#include "../../kernel/managed_service.c"
static ManagedService test_service;
#define TEST_CHILD() service_thread(&test_service)
#define TEST_DEAD() service_thread_dead(&test_service)
#define TEST_WAIT(timeout) service_wait_thread_dead(&test_service, (timeout))
#define TEST_PROGRAM test_service.program
#else
#include "../../kernel/supervisor.c"
#define TEST_CHILD() supervisor_thread()
#define TEST_DEAD() supervisor_thread_dead()
#define TEST_WAIT(timeout) supervisor_wait_thread_dead(timeout)
#define TEST_PROGRAM g_supervisor.program
#endif
static int irq_disabled, pending_completion, ticks_calls, schedules, mode;
static void complete(void) {
    Thread *t = TEST_CHILD();
    t->state = THREAD_STATE_DEAD;
    t->on_run_queue = false;
    t->interrupt_context_ready = false;
    t->interrupt_rsp = 0;
}
void interrupts_disable(void) { irq_disabled = 1; }
void interrupts_enable(void) {
    irq_disabled = 0;
    if (pending_completion) { pending_completion = 0; complete(); }
}
bool timer_initialized(void) { return true; }
u64 timer_ticks(void) {
    ++ticks_calls;
    if (mode == 1 && ticks_calls == 2) {
        if (irq_disabled) pending_completion = 1;
        else complete();
    }
    return mode == 3 ? (u64)ticks_calls * 10ULL : (u64)ticks_calls;
}
bool scheduler_yield(void) {
    ++schedules;
    if (mode == 2) return false;
    if (mode == 3) return true;
    complete();
    return true;
}
static void reset(int scenario) {
    mode = scenario;
    irq_disabled = pending_completion = ticks_calls = schedules = 0;
    TEST_PROGRAM.thread_created = true;
    Thread *t = TEST_CHILD();
    t->state = THREAD_STATE_READY;
    t->on_run_queue = true;
    t->interrupt_context_ready = true;
    t->interrupt_rsp = 1;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"supervisor wait line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    reset(1);
    CHECK(TEST_WAIT(100));
    CHECK(TEST_DEAD() && !irq_disabled);
    reset(0);
    CHECK(TEST_WAIT(100) && schedules == 1 && !irq_disabled);
    reset(0); complete();
    CHECK(TEST_WAIT(100) && !schedules && !irq_disabled);
    reset(2);
    CHECK(!TEST_WAIT(100) && !irq_disabled);
    reset(3);
    CHECK(!TEST_WAIT(15) && !irq_disabled);
    reset(0);
    CHECK(!TEST_WAIT(0) && !schedules);
    puts("Service terminal wait: PASS (preemption completion boundary, scheduled/already-dead, schedule failure, timeout)");
    return 0;
}
