#include "scheduler.h"
#include "arch.h"

static Thread *g_run_head;
static Thread *g_run_tail;

static u64 g_run_count;
static bool g_initialized;

bool scheduler_init(void) {
    if (g_initialized) return false;

    Thread *current = thread_current();

    if (!current || !current->id || current->state != THREAD_STATE_RUNNING || current->on_run_queue) return false;

    /* The bootstrap thread starts as the only runnable thread. */
    current->run_next = current;
    current->on_run_queue = true;

    g_run_head = current;
    g_run_tail = current;
    g_run_count = 1;
    g_initialized = true;

    return true;
}

bool scheduler_add(Thread *thread) {
    if (!g_initialized || !thread || !thread->id || thread->on_run_queue || thread->state != THREAD_STATE_READY) return false;
    if (!g_run_head || !g_run_tail || !g_run_count) return false;

    /*
     * Circular run queue:
     *
     * tail -> new -> head
     */
    thread->run_next = g_run_head;
    g_run_tail->run_next = thread;
    g_run_tail = thread;
    thread->on_run_queue = true;
    ++g_run_count;

    return true;
}

static bool scheduler_unlink(Thread *thread) {
    if (!thread || !thread->on_run_queue || !g_run_head || !g_run_tail || !g_run_count) return false;
    Thread *previous = g_run_head;
    bool found = false;

    for (u64 i = 0; i < g_run_count; ++i) {
        if (!previous || !previous->run_next) return false;
        if (previous->run_next == thread) {

            found = true;
            break;
        }
        previous = previous->run_next;
    }

    if (!found) return false;
    if (g_run_count == 1) {
        if (g_run_head != thread || g_run_tail != thread) return false;
        g_run_head = 0;
        g_run_tail = 0;
    } else {
        previous->run_next = thread->run_next;
        if (g_run_head == thread) g_run_head = thread->run_next;
        if (g_run_tail == thread) g_run_tail = previous;
        g_run_tail->run_next = g_run_head;
    }

    thread->run_next = 0;
    thread->on_run_queue = false;
    --g_run_count;

    return true;
}

bool scheduler_remove(Thread *thread) {
   if (!g_initialized || !thread || !thread->on_run_queue) return false;
    if (thread == thread_current()) return false;
    return scheduler_unlink(thread);
}

bool scheduler_yield(void) {
    if (!g_initialized || !g_run_head || !g_run_tail || !g_run_count) return false;

    Thread *current = thread_current();
    if (!current || !current->on_run_queue || current->state != THREAD_STATE_RUNNING || !current->run_next) return false;

    /* Search forward in round-robin order for another READY thread. */
    Thread *candidate = current->run_next;
    for (u64 inspected = 0; inspected + 1ULL < g_run_count; ++inspected) {
        if (!candidate) return false;
        if (candidate != current && candidate->on_run_queue && candidate->state == THREAD_STATE_READY) return thread_switch(candidate);
        candidate = candidate->run_next;
    }
    /* No other runnable thread. */
    return true;
}

u64 scheduler_thread_count(void) {
    return g_initialized ? g_run_count : 0;
}

NORETURN void scheduler_exit_current(void) {
    if (!g_initialized || !g_run_head || !g_run_tail || g_run_count < 2) cpu_halt_forever();

    Thread *dying = thread_current();
    if (!dying || !dying->id || !dying->on_run_queue || dying->state != THREAD_STATE_RUNNING || !dying->run_next) cpu_halt_forever();

    /* Find another READY thread whose saved context can actually be entered. */
    Thread *next = dying->run_next;
    Thread *chosen = 0;

    for (u64 inspected = 0; inspected + 1ULL < g_run_count; ++inspected) {
        if (!next) cpu_halt_forever();
        if (next != dying && next->on_run_queue && next->state == THREAD_STATE_READY && next->context_ready) {
            chosen = next;
            break;
        }
        next = next->run_next;
    }

    if (!chosen) cpu_halt_forever();

    /*
     * This installs:
     *
     *   chosen CR3
     *   chosen TSS.RSP0
     *   chosen as current
     *
     * It temporarily changes dying from
     * RUNNING to READY.
     */
    if (!thread_activate(chosen)) cpu_halt_forever();

    /* thread_current() is now chosen, so the dying thread may safely leave the queue. */
    if (!scheduler_unlink(dying)) cpu_halt_forever();

    dying->state = THREAD_STATE_DEAD;

    /* Its old cooperative context must never become runnable again. */
    dying->context_ready = false;

    /*
     * Do not free dying->kernel_stack here.
     *
     * We are still physically executing on it
     * until arch_context_enter changes RSP.
     */
    arch_context_enter(&chosen->context);
    cpu_halt_forever();
}