#include "interrupt_state_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "input.h"
#include "interrupts.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"

#define INTERRUPT_STATE_RFLAGS_IF (1ULL << 9)

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

static bool if_enabled(void) {
    return (read_rflags() & INTERRUPT_STATE_RFLAGS_IF) != 0;
}

static bool supervisor_ok(void) {
    u64 reply = 0;
    return supervisor_ping(0x4952515354415445ULL, &reply) && reply == 0x4952515354415445ULL;
}

static bool core_observers_preserve(bool expected_enabled) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    (void)pmm_stats();
    (void)process_object_count();
    (void)address_space_object_count();
    (void)thread_object_count();
    (void)endpoint_object_count();
    (void)capability_table_object_count();
    (void)capability_table_count(caps);
    (void)process_thread_count(kernel);

    return if_enabled() == expected_enabled;
}

void interrupt_state_test_run(void) {
    terminal_writeln("INTERRUPT STATE / CRITICAL SECTION TEST:");

    Thread *main = thread_current();
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;

    bool preflight = main && kernel && caps && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled() && if_enabled() && supervisor_ok();
    report("SINGLE RUNNABLE / IF=1 BASELINE", preflight);
    if (!preflight) return;

    u64 free_before = pmm_stats().free_pages;
    u32 process_before = process_object_count();
    u32 space_before = address_space_object_count();
    u32 thread_before = thread_object_count();
    u32 endpoint_before = endpoint_object_count();
    u32 table_before = capability_table_object_count();
    u32 caps_before = capability_table_count(caps);
    u64 kernel_threads_before = process_thread_count(kernel);

    KeyEvent event;
    k_memset(&event, 0, sizeof(event));
    (void)input_poll(&event);
    bool input_enabled_ok = if_enabled();
    report("INPUT POLL PRESERVES IF=1", input_enabled_ok);

    bool observers_enabled_ok = core_observers_preserve(true);
    report("CORE OBSERVERS PRESERVE IF=1", observers_enabled_ok);

    interrupts_disable();
    bool entered_disabled = !if_enabled();
    report("ENTER IF=0", entered_disabled);

    KeyEvent disabled_event;
    k_memset(&disabled_event, 0, sizeof(disabled_event));
    (void)input_poll(&disabled_event);
    bool input_disabled_ok = !if_enabled();
    report("INPUT POLL PRESERVES IF=0", input_disabled_ok);

    bool observers_disabled_ok = core_observers_preserve(false);
    report("CORE OBSERVERS PRESERVE IF=0", observers_disabled_ok);

    interrupts_enable();
    bool restored_enabled = if_enabled();
    report("RESTORE IF=1", restored_enabled);

    bool baseline = input_enabled_ok && observers_enabled_ok && entered_disabled && input_disabled_ok &&
        observers_disabled_ok && restored_enabled && thread_current() == main &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue && scheduler_thread_count() == 1ULL &&
        !scheduler_preemption_enabled() && pmm_stats().free_pages == free_before &&
        process_object_count() == process_before && address_space_object_count() == space_before &&
        thread_object_count() == thread_before && endpoint_object_count() == endpoint_before &&
        capability_table_object_count() == table_before && capability_table_count(caps) == caps_before &&
        process_thread_count(kernel) == kernel_threads_before && supervisor_ok();
    report("RESOURCE / SCHEDULER / SUPERVISOR BASELINES", baseline);

    bool pass = preflight && baseline;
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("INTERRUPT STATE / CRITICAL SECTION TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
