#include "scheduler_idle_test.h"

#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "ipc_timeout_test.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"

#define IDLE_TEST_TIMEOUT_TICKS 2ULL
#define RFLAGS_IF (1ULL << 9)

static Endpoint g_endpoint;
static CapabilityHandle g_receive;
static bool g_endpoint_created;
static bool g_receive_cap;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static u64 read_rflags(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    return flags;
}

static bool message_zero(const IpcMessage *message) {
    if (!message || message->word_count) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) if (message->words[i]) return false;
    return true;
}


static bool idle_until_tick(u64 *wait_delta) {
    if (wait_delta) *wait_delta = 0;
    u64 ticks = timer_ticks();
    u64 waits = scheduler_idle_wait_count();

    /* IF may already be set here. PIT can therefore advance the tick after the
     * snapshot above but before a loop condition is evaluated. Always execute
     * at least one scheduler wait so this test proves the idle-wait path rather
     * than occasionally observing a tick that happened just before it. */
    for (u32 i = 0; i < 16U; ++i) {
        if (!scheduler_wait_current()) return false;
        if (timer_ticks() != ticks) break;
    }
    u64 delta = scheduler_idle_wait_count() - waits;
    if (wait_delta) *wait_delta = delta;
    return timer_ticks() != ticks && delta > 0ULL;
}

static bool supervisor_ok(void) {
    u64 reply = 0;
    return supervisor_ping(0x49444C4557414954ULL, &reply) && reply == 0x49444C4557414954ULL;
}

static bool cleanup(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (g_receive_cap) {
        if (!caps || !capability_revoke(caps, g_receive)) return false;
        g_receive_cap = false;
    }
    if (g_endpoint_created) {
        if (g_endpoint.message_ready) {
            IpcMessage discard;
            if (!endpoint_try_receive(&g_endpoint, &discard)) return false;
        }
        if (g_endpoint.waiting_receiver || g_endpoint.waiting_receiver_id ||
            g_endpoint.waiting_sender || g_endpoint.waiting_sender_id ||
            g_endpoint.waiting_sender_message_ready) return false;
        if (!endpoint_destroy(&g_endpoint)) return false;
        g_endpoint_created = false;
    }
    return true;
}

void scheduler_idle_test_cleanup_run(void) {
    terminal_writeln("SCHEDULER IDLE CLEANUP RETRY:");
    report("RETAINED RESOURCES RELEASED", cleanup());
}

void scheduler_idle_test_run(void) {
    terminal_writeln("SCHEDULER LAST-RUNNABLE IDLE TEST:");

    if (!cleanup()) {
        report("NO RETAINED FIXTURE", false);
        return;
    }

    Process *kernel = process_kernel();
    Thread *main = thread_current();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    bool preflight = kernel && main && caps && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled() &&
        (read_rflags() & RFLAGS_IF) && timer_initialized() && !ipc_timeout_test_active_count() && supervisor_ok();
    report("SINGLE RUNNABLE / TIMER / SUPERVISOR", preflight);
    if (!preflight) return;

    bool timed_out_in_idle = false;
    u64 frames_before = pmm_stats().free_pages;
    u32 endpoints_before = endpoint_object_count();
    u32 caps_before = capability_table_count(caps);
    u64 kernel_threads_before = process_thread_count(kernel);
    u32 thread_objects_before = thread_object_count();

    u64 flags_before = read_rflags();
    u64 direct_waits = 0;
    bool enabled_wait = (flags_before & RFLAGS_IF) && idle_until_tick(&direct_waits);
    u64 flags_after = read_rflags();
    bool enabled_preserved = enabled_wait && direct_waits > 0ULL && (flags_after & RFLAGS_IF);
    report("IF=1 IDLE WAIT / RESTORE", enabled_preserved);
    if (!enabled_preserved) goto done;

    interrupts_disable();
    direct_waits = 0;
    bool disabled_wait = !(read_rflags() & RFLAGS_IF) && idle_until_tick(&direct_waits);
    bool disabled_preserved = disabled_wait && direct_waits > 0ULL && !(read_rflags() & RFLAGS_IF);
    if (flags_before & RFLAGS_IF) interrupts_enable();
    report("IF=0 IDLE WAIT / RESTORE", disabled_preserved);
    if (!disabled_preserved) goto done;

    if (!endpoint_create(&g_endpoint)) goto done;
    g_endpoint_created = true;
    if (!capability_insert(caps, &g_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_receive)) goto done;
    g_receive_cap = true;

    IpcMessage output;
    k_memset(&output, 0xA5, sizeof(output));
    u64 ticks_before = timer_ticks();
    u64 idle_before = scheduler_idle_wait_count();
    bool received = ipc_receive_blocking_for(kernel, g_receive, &output, IDLE_TEST_TIMEOUT_TICKS);
    u64 tick_delta = timer_ticks() - ticks_before;
    u64 idle_delta = scheduler_idle_wait_count() - idle_before;

    timed_out_in_idle = !received && tick_delta >= IDLE_TEST_TIMEOUT_TICKS && idle_delta > 0ULL &&
        message_zero(&output) && !thread_wait_active(main) && !ipc_timeout_test_active_count() &&
        !g_endpoint.waiting_receiver && !g_endpoint.waiting_receiver_id &&
        scheduler_thread_count() == 1ULL && thread_current() == main &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue;
    report("LAST RUNNABLE TIMED RECEIVE", timed_out_in_idle);
    if (!timed_out_in_idle) goto done;

done: {
        bool released = cleanup();
        bool baseline = released && scheduler_thread_count() == 1ULL && thread_current() == main &&
            main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
            !scheduler_preemption_enabled() && !thread_wait_active(main) &&
            !ipc_timeout_test_active_count() && pmm_stats().free_pages == frames_before &&
            endpoint_object_count() == endpoints_before && capability_table_count(caps) == caps_before &&
            process_thread_count(kernel) == kernel_threads_before &&
            thread_object_count() == thread_objects_before && supervisor_ok();
        report("RESOURCE / SCHEDULER / SUPERVISOR BASELINES", baseline);
        terminal_set_color(timed_out_in_idle && baseline ? terminal_accent_color() : terminal_error_color());
        terminal_write("SCHEDULER LAST-RUNNABLE IDLE TEST: ");
        terminal_writeln(timed_out_in_idle && baseline ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
        if (!released) terminal_writeln("IDLE TEST FIXTURE RETAINED. REBOOT IF CLEANUP RETRY FAILS.");
    }
}
