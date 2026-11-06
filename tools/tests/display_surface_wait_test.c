/* Exercise the actual graphics diagnostic wait with preemption completion.
 * No display output, real scheduler, device or process is constructed. */
#include <stdio.h>
#include "../../kernel/tests/display_surface_test.c"
static int irq_disabled, pending_completion, calls, schedules, mode, waiting;
static u64 ticks;
static Thread *child(void) { return &g_input_apps[0].thread; }
static void complete(void) {
    Thread *t=child();
    t->state=THREAD_STATE_DEAD; t->on_run_queue=false;
    t->interrupt_context_ready=false; t->interrupt_rsp=0; waiting=0;
    g_input_exit_queues[0].pending_count=1; g_input_exit_queues[0].watch_count=0;
}
void interrupts_disable(void) { irq_disabled=1; }
void interrupts_enable(void) {
    irq_disabled=0;
    if (pending_completion) { pending_completion=0; complete(); }
}
bool timer_initialized(void) { return true; }
u32 timer_frequency(void) { return 100; }
u64 timer_ticks(void) { return ++ticks; }
bool thread_wait_active(const Thread *t) { (void)t; return waiting!=0; }
u32 process_exit_queue_pending_count(const ProcessExitQueue *q) {
    u32 result=q->pending_count;
    if (mode==1 && !calls++) {
        if (irq_disabled) pending_completion=1;
        else complete(); /* Old code returns a stale queue count to its caller. */
    }
    return result;
}
u32 process_exit_queue_watch_count(const ProcessExitQueue *q) { return q->watch_count; }
bool scheduler_yield(void) {
    ++schedules;
    if (mode==3 || child()->state==THREAD_STATE_DEAD) return false;
    if (mode==4) { ticks+=100; return true; }
    pending_completion=0; complete(); return true;
}
static void reset(int scenario) {
    mode=scenario; irq_disabled=pending_completion=calls=schedules=waiting=0; ticks=0;
    g_input_exit_queue_created[0]=true;
    g_input_exit_queues[0].pending_count=0; g_input_exit_queues[0].watch_count=1;
    Thread *t=child(); t->state=THREAD_STATE_READY; t->on_run_queue=true;
    t->interrupt_context_ready=true; t->interrupt_rsp=1;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"client-exit wait line%d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    reset(1);
    CHECK(wait_input_app_dead(0) && input_app_terminal(0) && !irq_disabled);
    reset(0);
    CHECK(wait_input_app_dead(0) && schedules==1 && !irq_disabled);
    reset(2); complete();
    CHECK(wait_input_app_dead(0) && !schedules && !irq_disabled);
    reset(3);
    CHECK(!wait_input_app_dead(0) && !irq_disabled);
    reset(4);
    CHECK(!wait_input_app_dead(0) && !irq_disabled);
    reset(5); g_input_exit_queues[0].pending_count=2;
    CHECK(!wait_input_app_dead(0) && !schedules && !irq_disabled);
    reset(6); child()->on_run_queue=false;
    CHECK(!wait_input_app_dead(0) && !schedules && !irq_disabled);
    reset(7); complete(); g_input_exit_queues[0].watch_count=1;
    CHECK(!wait_input_app_dead(0) && !irq_disabled);
    reset(8); child()->state=THREAD_STATE_BLOCKED; child()->on_run_queue=false; waiting=1;
    CHECK(wait_input_app_dead(0) && schedules==1 && !irq_disabled);
    CHECK(!wait_input_app_dead(2));
    puts("PASS: production focused-client wait; completion between queue/thread observations, already dead, scheduled wake, blocked IPC, timeout, schedule failure and malformed terminal state.");
    return 0;
}
