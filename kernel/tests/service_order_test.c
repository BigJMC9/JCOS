#include "service_order_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "supervisor.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"

#define SERVICE_ORDER_COOKIE 0x5352564F52444552ULL
#define SERVICE_ORDER_CLEANUP_COOKIE 0x535256434C45414EULL
#define SERVICE_ORDER_RFLAGS_IF (1ULL << 9)

static Thread g_spinner;
static bool g_spinner_created;
static volatile u64 g_spinner_count;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void irq_restore(u64 flags) {
    if (flags & SERVICE_ORDER_RFLAGS_IF) interrupts_enable();
}

static void spinner_entry(void *argument) {
    volatile u64 *counter = (volatile u64 *)argument;
    for (;;) {
        ++*counter;
        __asm__ volatile ("" : : : "memory");
    }
}

static bool cleanup_spinner(void) {
    if (!g_spinner_created) return true;
    if (thread_current() == &g_spinner || g_spinner.state == THREAD_STATE_RUNNING) return false;

    if (g_spinner.on_run_queue || g_spinner.state == THREAD_STATE_BLOCKED) {
        if (!task_terminate_thread(&g_spinner)) return false;
    }
    if (g_spinner.on_run_queue || thread_wait_active(&g_spinner)) return false;
    if (thread_storage_in_use(&g_spinner) && !thread_destroy(&g_spinner)) return false;

    g_spinner_created = false;
    k_memset(&g_spinner, 0, sizeof(g_spinner));
    return true;
}

static bool supervisor_healthy(void) {
    if (!supervisor_running()) return false;
    u64 reply = 0;
    return supervisor_stack_guarded() &&
        supervisor_ping(SERVICE_ORDER_CLEANUP_COOKIE, &reply) &&
        reply == SERVICE_ORDER_CLEANUP_COOKIE;
}

static bool restore_supervisor(void) {
    if (supervisor_running()) return supervisor_healthy();
    if (scheduler_thread_count() != 1ULL) return false;
    if (!supervisor_start()) return false;
    return supervisor_healthy();
}

static bool cleanup_all(void) {
    u64 flags = irq_save();
    bool spinner = cleanup_spinner();
    irq_restore(flags);
    if (!spinner) return false;
    return restore_supervisor();
}

void service_order_test_cleanup_run(void) {
    terminal_writeln("SERVICE ORDER CLEANUP RETRY:");
    report("SPINNER RELEASED / SUPERVISOR AVAILABLE", cleanup_all());
}

void service_order_test_run(void) {
    terminal_writeln("SERVICE ORDER / BOUNDED SHUTDOWN TEST:");

    bool prior_cleanup = cleanup_all();
    Thread *main = thread_current();
    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    bool preflight = prior_cleanup && main && kernel && kernel_caps && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue && scheduler_thread_count() == 1ULL &&
        scheduler_preemption_enabled() && timer_initialized() && supervisor_healthy();
    report("NORMAL PREEMPTION / SERVICE BASELINE", preflight);
    if (!preflight) goto finish;

    PmmStats before = pmm_stats();
    u32 processes_before = process_object_count();
    u32 spaces_before = address_space_object_count();
    u32 threads_before = thread_object_count();
    u32 endpoints_before = endpoint_object_count();
    u32 caps_before = capability_table_count(kernel_caps);
    u64 kernel_threads_before = process_thread_count(kernel);

    g_spinner_count = 0;
    bool created = thread_create(&g_spinner, kernel);
    g_spinner_created = created;
    bool prepared = created && thread_prepare_kernel(&g_spinner, spinner_entry, (void *)&g_spinner_count);

    u64 flags = irq_save();
    bool queued = prepared && scheduler_add(&g_spinner);
    irq_restore(flags);
    report("SPINNER QUEUED AHEAD OF SHUTDOWN", created && prepared && queued);
    if (!queued) goto finish;

    u64 preempt_before = scheduler_preemption_count();
    u64 ticks_before = timer_ticks();
    bool stopped = supervisor_stop();
    u64 spin_count = g_spinner_count;
    u64 preempt_delta = scheduler_preemption_count() - preempt_before;
    u64 tick_delta = timer_ticks() - ticks_before;
    bool service_released = stopped && !supervisor_running();

    report("SPINNER RAN / PIT PREEMPTED", spin_count > 0ULL && preempt_delta > 0ULL && tick_delta > 0ULL);
    report("BOUNDED SUPERVISOR STOP THROUGH CONTENTION", service_released);

    flags = irq_save();
    bool spinner_released = cleanup_spinner();
    bool queue_restored = spinner_released && scheduler_thread_count() == 1ULL &&
        thread_current() == main && main->state == THREAD_STATE_RUNNING && main->on_run_queue;
    irq_restore(flags);
    report("SPINNER CLEANUP / RUN QUEUE RESTORED", queue_restored);

    bool restarted = service_released && queue_restored && supervisor_start();
    u64 reply = 0;
    bool ping = restarted && supervisor_ping(SERVICE_ORDER_COOKIE, &reply) && reply == SERVICE_ORDER_COOKIE &&
        supervisor_running() && supervisor_stack_guarded();
    report("SUPERVISOR RESTART / PING", ping);

    PmmStats after = pmm_stats();
    bool baselines = ping && scheduler_preemption_enabled() && scheduler_thread_count() == 1ULL &&
        thread_current() == main && process_object_count() == processes_before &&
        address_space_object_count() == spaces_before && thread_object_count() == threads_before &&
        endpoint_object_count() == endpoints_before && capability_table_count(kernel_caps) == caps_before &&
        process_thread_count(kernel) == kernel_threads_before && after.free_pages == before.free_pages;
    report("RESOURCE / PREEMPTION BASELINES", baselines);

    if (spin_count > 0ULL && preempt_delta > 0ULL && tick_delta > 0ULL && service_released &&
        queue_restored && ping && baselines) {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("SERVICE ORDER / BOUNDED SHUTDOWN TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

finish:
    {
        bool restored = cleanup_all();
        report("FAILURE CLEANUP / SUPERVISOR RESTORED", restored);
        terminal_set_color(terminal_error_color());
        terminal_writeln("SERVICE ORDER / BOUNDED SHUTDOWN TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}
