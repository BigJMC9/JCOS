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
    if (!g_initialized || !thread || !thread->id || thread->on_run_queue || !thread->context_ready || thread->state != THREAD_STATE_READY) return false;
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
        if (candidate != current && candidate->on_run_queue && candidate->context_ready && candidate->state == THREAD_STATE_READY) return thread_switch(candidate);

        candidate = candidate->run_next;
    }
    /* No other runnable thread. */
    return true;
}

bool scheduler_block_current(void) {
    if (!g_initialized || !g_run_head || !g_run_tail || g_run_count < 2) return false;

    Thread *blocked = thread_current();

    if (!blocked || !blocked->id || !blocked->on_run_queue || blocked->state != THREAD_STATE_RUNNING || !blocked->run_next) return false;

    /*
     * Find another runnable thread.
     *
     * Its context must already be enterable.
     */
    Thread *candidate = blocked->run_next;
    Thread *chosen = 0;

    for (u64 inspected = 0; inspected + 1ULL < g_run_count; ++inspected) {
        if (!candidate) return false;
        if (candidate != blocked && candidate->on_run_queue && candidate->context_ready && candidate->state == THREAD_STATE_READY) {

            chosen = candidate;
            break;
        }
        candidate = candidate->run_next;
    }

    if (!chosen) return false;

    /*
     * First install the target thread's:
     *
     *   current-thread identity
     *   CR3
     *   TSS.RSP0
     *
     * thread_activate() temporarily changes
     * blocked from RUNNING to READY.
     */
    if (!thread_activate(chosen)) return false;

    /*
     * Still executing on blocked's
     * kernel stack, but chosen is now the
     * architectural current thread.
     *
     * Interrupts must remain disabled across
     * this transition.
     */
    if (!scheduler_unlink(blocked)) cpu_halt_forever();

    blocked->state = THREAD_STATE_BLOCKED;

    /* arch_context_switch() is about to save the continuation at this exact point. */
    blocked->context_ready = true;
    arch_context_switch(&blocked->context, &chosen->context);

    /* Reached only after scheduler_wake() made this thread READY again and a later scheduler switch selected it. */
    return thread_current() == blocked && blocked->state == THREAD_STATE_RUNNING && blocked->on_run_queue;
}

bool scheduler_wake(Thread *thread) {
    if (!g_initialized ||
        !thread ||
        !thread->id ||
        thread == thread_current() ||
        thread->on_run_queue ||
        thread->run_next ||
        !thread->context_ready ||
        thread->state != THREAD_STATE_BLOCKED) {

        return false;
    }

    /*
     * scheduler_add() accepts READY threads.
     * Restore BLOCKED if insertion somehow
     * fails.
     */
    thread->state = THREAD_STATE_READY;
    if (!scheduler_add(thread)) {
        thread->state = THREAD_STATE_BLOCKED;
        return false;
    }
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