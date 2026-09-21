#include "process_exit_queue_test.h"

#include "address_space.h"
#include "arch.h"
#include "capability.h"
#include "interrupts.h"
#include "lib.h"
#include "pmm.h"
#include "program.h"
#include "process.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"
#include "vfs.h"

#define EXIT_QUEUE_TEST_RFLAGS_IF (1ULL << 9)

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 threads;
    u32 spaces;
    u32 queues;
    u64 runnable;
} ExitQueueBaseline;

typedef enum {
    HELPER_QUIESCE_TWO = 0,
    HELPER_CLOSE_QUEUE,
    HELPER_WAIT_QUEUE
} HelperAction;

typedef struct {
    HelperAction action;
    ProcessExitQueue *queue;
    Process *first;
    Process *second;
    bool first_ok;
    bool second_ok;
    bool wait_returned;
    bool wait_result;
} HelperContext;

static ProcessExitQueue g_queue;
static Process g_children[2];
static Thread g_child_threads[2];
static Thread g_helper;
static HelperContext g_context;

static u64 test_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void test_irq_restore(u64 flags) {
    if (flags & EXIT_QUEUE_TEST_RFLAGS_IF) interrupts_enable();
}

static bool check(const char *name, bool pass) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    return pass;
}

static ExitQueueBaseline baseline_take(void) {
    ExitQueueBaseline b;
    b.free_pages = pmm_stats().free_pages;
    b.processes = process_object_count();
    b.threads = thread_object_count();
    b.spaces = address_space_object_count();
    b.queues = process_exit_queue_object_count();
    b.runnable = scheduler_thread_count();
    return b;
}

static bool baseline_matches(const ExitQueueBaseline *b) {
    return b && pmm_stats().free_pages == b->free_pages &&
        process_object_count() == b->processes && thread_object_count() == b->threads &&
        address_space_object_count() == b->spaces && process_exit_queue_object_count() == b->queues &&
        scheduler_thread_count() == b->runnable;
}

static void helper_entry(void *argument) {
    HelperContext *ctx = (HelperContext *)argument;
    if (!ctx || !ctx->queue) cpu_halt_forever();
    if (ctx->action == HELPER_QUIESCE_TWO) {
        ctx->first_ok = ctx->first && task_quiesce_process(ctx->first);
        ctx->second_ok = ctx->second && task_quiesce_process(ctx->second);
    } else if (ctx->action == HELPER_CLOSE_QUEUE) {
        ctx->first_ok = process_exit_queue_close(ctx->queue);
    } else if (ctx->action == HELPER_WAIT_QUEUE) {
        ProcessExitInfo event;
        ctx->wait_result = process_exit_queue_receive_blocking(ctx->queue, &event);
        ctx->wait_returned = true;
    }
    scheduler_exit_current();
}

static bool helper_start(HelperAction action, ProcessExitQueue *queue, Process *first, Process *second) {
    k_memset(&g_helper, 0, sizeof(g_helper));
    k_memset(&g_context, 0, sizeof(g_context));
    g_context.action = action;
    g_context.queue = queue;
    g_context.first = first;
    g_context.second = second;
    Process *kernel = process_kernel();
    return kernel && thread_create(&g_helper, kernel) &&
        thread_prepare_kernel(&g_helper, helper_entry, &g_context) && scheduler_add(&g_helper);
}

static bool helper_reap(void) {
    return g_helper.state == THREAD_STATE_DEAD && !g_helper.on_run_queue &&
        !thread_wait_active(&g_helper) && thread_destroy(&g_helper);
}

static bool child_create(u32 index, ProcessExitQueue *queue, u64 *pid, u64 *tid) {
    if (index >= 2U || !queue || !pid || !tid) return false;
    k_memset(&g_children[index], 0, sizeof(g_children[index]));
    k_memset(&g_child_threads[index], 0, sizeof(g_child_threads[index]));
    if (!process_create(&g_children[index]) || !process_exit_queue_watch(queue, &g_children[index]) ||
        !thread_create(&g_child_threads[index], &g_children[index])) return false;
    *pid = g_children[index].id;
    *tid = g_child_threads[index].id;
    return *pid && *tid;
}

static bool event_matches(const ProcessExitInfo *event, u64 pid, u64 tid) {
    return event && event->reason == PROCESS_EXIT_TERMINATED &&
        event->process_id == pid && event->thread_id == tid &&
        !event->vector && !event->error_code && !event->rip && !event->cr2;
}


static bool launcher_case(void) {
    ExitQueueBaseline base = baseline_take();
    k_memset(&g_queue, 0, sizeof(g_queue));
    ProgramInstance instance;
    ProgramLaunchSpec spec;
    k_memset(&instance, 0, sizeof(instance));
    k_memset(&spec, 0, sizeof(spec));

    VfsNode *file = vfs_resolve(vfs_root(), "/bin/elftest.elf");
    bool pass = file && process_exit_queue_create(&g_queue);
    if (pass) {
        spec.file = file;
        spec.exit_queue = &g_queue;
        pass = program_launch(&instance, &spec);
    }
    u64 pid = instance.process.id;
    u64 tid = instance.thread.id;
    bool reserved = pass && pid && tid && instance.published &&
        process_exit_queue_watch_count(&g_queue) == 1U;
    pass = check("LAUNCHER RESERVED EXIT SLOT BEFORE PUBLICATION", reserved) && pass;

    if (pass) {
        u64 flags = test_irq_save();
        pass = scheduler_yield();
        test_irq_restore(flags);
    }
    ProcessExitInfo event;
    k_memset(&event, 0, sizeof(event));
    bool delivered = pass && instance.thread.state == THREAD_STATE_DEAD &&
        process_exit_queue_try_receive(&g_queue, &event) &&
        event.reason == PROCESS_EXIT_NORMAL && event.process_id == pid && event.thread_id == tid;
    pass = check("NORMAL LAUNCH EXIT DELIVERED", delivered) && pass;

    if (pass) pass = program_reap(&instance) && !program_instance_needs_cleanup(&instance);
    pass = check("WATCHED PROGRAM REAP", pass) && pass;

    bool closed = pass && process_exit_queue_close(&g_queue);
    k_memset(&instance, 0, sizeof(instance));
    bool rejected = false;
    if (closed) {
        k_memset(&spec, 0, sizeof(spec));
        spec.file = file;
        spec.exit_queue = &g_queue;
        rejected = !program_launch(&instance, &spec) && !program_instance_needs_cleanup(&instance) &&
            process_exit_queue_pending_count(&g_queue) == 0U;
    }
    pass = check("CLOSED QUEUE REJECTS LAUNCH WITHOUT FALSE EXIT", rejected) && pass;

    if (pass) pass = process_exit_queue_destroy(&g_queue);
    pass = check("LAUNCHER CASE BASELINE", pass && baseline_matches(&base)) && pass;
    return pass;
}

static bool two_event_case(void) {
    ExitQueueBaseline base = baseline_take();
    k_memset(&g_queue, 0, sizeof(g_queue));
    bool pass = process_exit_queue_create(&g_queue);
    pass = check("QUEUE CREATE", pass) && pass;

    u64 pid0 = 0, pid1 = 0, tid0 = 0, tid1 = 0;
    if (pass) pass = child_create(0U, &g_queue, &pid0, &tid0) && child_create(1U, &g_queue, &pid1, &tid1);
    pass = check("TWO WATCHED PROCESS INCARNATIONS", pass && process_exit_queue_watch_count(&g_queue) == 2U) && pass;

    if (pass) pass = helper_start(HELPER_QUIESCE_TWO, &g_queue, &g_children[0], &g_children[1]);
    pass = check("RECOVERY HELPER RUNNABLE", pass) && pass;

    ProcessExitInfo first;
    k_memset(&first, 0, sizeof(first));
    bool received = pass && process_exit_queue_receive_blocking(&g_queue, &first);
    bool blocked_woke = received && event_matches(&first, pid0, tid0) &&
        g_context.first_ok && g_context.second_ok && process_exit_queue_watch_count(&g_queue) == 0U &&
        process_exit_queue_pending_count(&g_queue) == 1U;
    pass = check("BLOCKED OWNER WOKE / SECOND EVENT RETAINED", blocked_woke) && pass;

    if (pass) pass = helper_reap();
    pass = check("HELPER REAP", pass) && pass;

    if (pass) pass = process_destroy(&g_children[0]);
    u64 old_pid1 = pid1;
    if (pass) pass = process_destroy(&g_children[1]) && process_create(&g_children[1]);
    u64 replacement_pid = g_children[1].id;
    bool reused = pass && replacement_pid && replacement_pid != old_pid1;
    pass = check("PROCESS STORAGE REUSED WITH NEW ID", reused) && pass;

    ProcessExitInfo second;
    k_memset(&second, 0, sizeof(second));
    bool second_received = pass && process_exit_queue_try_receive(&g_queue, &second);
    bool stale_safe = second_received && event_matches(&second, old_pid1, tid1) &&
        second.process_id != replacement_pid && process_exit_queue_pending_count(&g_queue) == 0U;
    pass = check("PENDING EVENT KEEPS OLD INCARNATION ID", stale_safe) && pass;

    bool watch_then_destroy = pass && process_exit_queue_watch(&g_queue, &g_children[1]) &&
        process_exit_queue_watch_count(&g_queue) == 1U && process_destroy(&g_children[1]) &&
        process_exit_queue_watch_count(&g_queue) == 0U;
    pass = check("NONTERMINAL DESTROY CANCELS WATCH", watch_then_destroy) && pass;

    bool queue_released = pass && process_exit_queue_close(&g_queue) && process_exit_queue_destroy(&g_queue);
    pass = check("QUEUE CLOSE / DESTROY", queue_released) && pass;
    pass = check("RESOURCE BASELINE", pass && baseline_matches(&base)) && pass;
    return pass;
}

static bool close_wakeup_case(void) {
    ExitQueueBaseline base = baseline_take();
    k_memset(&g_queue, 0, sizeof(g_queue));
    bool pass = process_exit_queue_create(&g_queue) && helper_start(HELPER_CLOSE_QUEUE, &g_queue, 0, 0);

    ProcessExitInfo event;
    k_memset(&event, 0xA5, sizeof(event));
    bool received = pass && process_exit_queue_receive_blocking(&g_queue, &event);
    bool zero = event.reason == PROCESS_EXIT_NONE && !event.process_id && !event.thread_id &&
        !event.vector && !event.error_code && !event.rip && !event.cr2;
    bool closed = !received && zero && g_context.first_ok && !g_queue.waiting_receiver &&
        !g_queue.waiting_receiver_id && !thread_wait_active(thread_current());
    pass = check("CLOSE WAKES BLOCKED OWNER WITH FAILURE", closed) && pass;

    if (pass) pass = helper_reap() && process_exit_queue_destroy(&g_queue);
    pass = check("CLOSE CASE CLEANUP", pass && baseline_matches(&base)) && pass;
    return pass;
}

static bool owner_abort_case(void) {
    ExitQueueBaseline base = baseline_take();
    k_memset(&g_queue, 0, sizeof(g_queue));
    bool pass = process_exit_queue_create(&g_queue) && helper_start(HELPER_WAIT_QUEUE, &g_queue, 0, 0);
    if (pass) {
        u64 flags = test_irq_save();
        pass = scheduler_yield();
        test_irq_restore(flags);
    }
    bool blocked = pass && g_helper.state == THREAD_STATE_BLOCKED && thread_wait_active(&g_helper) &&
        g_helper.wait_kind == THREAD_WAIT_PROCESS_EXIT && g_queue.waiting_receiver == &g_helper;
    pass = check("OWNER BLOCKED ON EXIT QUEUE", blocked) && pass;

    bool terminated = pass && task_terminate_thread(&g_helper) && g_helper.state == THREAD_STATE_DEAD &&
        !thread_wait_active(&g_helper) && !g_queue.waiting_receiver && !g_queue.waiting_receiver_id &&
        !g_context.wait_returned;
    pass = check("FORCED OWNER DEATH ABORTS QUEUE WAIT", terminated) && pass;

    if (pass) pass = thread_destroy(&g_helper) && process_exit_queue_close(&g_queue) &&
        process_exit_queue_destroy(&g_queue);
    pass = check("OWNER ABORT CLEANUP", pass && baseline_matches(&base)) && pass;
    return pass;
}

void process_exit_queue_test_run(void) {
    terminal_writeln("PROCESS EXIT OWNER NOTIFICATION TEST:");
    Thread *main = thread_current();
    Process *kernel = process_kernel();
    if (!main || !kernel || main->process != kernel || scheduler_thread_count() != 1ULL) {
        check("PREFLIGHT", false);
        return;
    }

    bool pass = launcher_case();
    pass = two_event_case() && pass;
    pass = close_wakeup_case() && pass;
    pass = owner_abort_case() && pass;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("PROCESS EXIT OWNER NOTIFICATION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
