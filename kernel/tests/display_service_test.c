#include "display_service_test.h"

#include "address_space.h"
#include "capability.h"
#include "display.h"
#include "display_service.h"
#include "endpoint.h"
#include "framebuffer.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "display_service_protocol.h"

#define DISPLAY_SERVICE_TEST_COOKIE 0x523842444953504CULL

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 address_spaces;
    u32 threads;
    u32 endpoints;
    u32 exit_queues;
    u32 kernel_caps;
    u64 kernel_threads;
    u64 scheduler_threads;
    bool terminal_render;
} DisplayServiceBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(DisplayServiceBaseline *out) {
    if (!out) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->address_spaces = address_space_object_count();
    out->threads = thread_object_count();
    out->endpoints = endpoint_object_count();
    out->exit_queues = process_exit_queue_object_count();
    out->kernel_caps = capability_table_count(caps);
    out->kernel_threads = process_thread_count(kernel);
    out->scheduler_threads = scheduler_thread_count();
    out->terminal_render = terminal_render_enabled();
    return true;
}

static bool baseline_matches(const DisplayServiceBaseline *baseline) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return kernel && caps &&
        pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->address_spaces &&
        thread_object_count() == baseline->threads &&
        endpoint_object_count() == baseline->endpoints &&
        process_exit_queue_object_count() == baseline->exit_queues &&
        capability_table_count(caps) == baseline->kernel_caps &&
        process_thread_count(kernel) == baseline->kernel_threads &&
        scheduler_thread_count() == baseline->scheduler_threads &&
        terminal_render_enabled() == baseline->terminal_render;
}

static bool cleanup_all(void) {
    return display_service_cleanup() &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL;
}

void display_service_test_cleanup_run(void) {
    terminal_writeln("R8B DISPLAY SERVICE CLEANUP RETRY:");
    report("SERVICE RELEASED / KERNEL DISPLAY RESTORED", cleanup_all());
}

void display_service_test_run(void) {
    terminal_writeln("R8B RING3 DISPLAY SERVICE TEST:");

    FramebufferInfo info;
    bool descriptor = framebuffer_info(&info) && info.physical_base && info.size &&
        info.width && info.height && info.pixels_per_scanline >= info.width;
    report("VALIDATED GOP DESCRIPTOR AVAILABLE", descriptor);
    if (!descriptor) goto fail;

    bool cleaned = cleanup_all();
    bool preflight = cleaned && scheduler_preemption_enabled() && timer_initialized() &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL;
    report("DISPLAY SERVICE / LEASE BASELINE", preflight);
    if (!preflight) goto fail;

    if (!info.direct_map_safe) {
        bool fallback = !display_service_start() &&
            display_service_state() == MANAGED_SERVICE_STOPPED &&
            display_direct_state() == DISPLAY_LEASE_KERNEL;
        report("NON-MMIO GOP / RING3 DIRECT MAP REFUSED", fallback);
        if (!fallback) goto fail;
        terminal_set_color(terminal_accent_color());
        terminal_writeln("R8B RING3 DISPLAY SERVICE TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

    DisplayServiceBaseline baseline;
    if (!capture_baseline(&baseline)) goto fail;

    bool started = display_service_start();
    u64 first_pid = display_service_process_id();
    u64 first_tid = display_service_thread_id();
    u64 first_incarnation = display_service_incarnation();
    bool lease_owned = started && first_pid && first_tid && first_incarnation &&
        display_service_running() && display_direct_state() == DISPLAY_LEASE_USER &&
        display_direct_owner_process_id() == first_pid && !terminal_render_enabled();
    report("RING3 SERVICE START / DIRECT GOP LEASE", lease_owned);
    if (!lease_owned) goto fail;

    bool retirement_refused = !display_retire_firmware_scanout() &&
        framebuffer_firmware_valid() && display_direct_state() == DISPLAY_LEASE_USER &&
        display_direct_owner_process_id() == first_pid;
    report("LIVE GOP MAPPING BLOCKS SCANOUT RETIREMENT", retirement_refused);
    if (!retirement_refused) goto fail;

    ProgramBorrowedMappingSpec competing_mapping;
    FramebufferInfo competing_info;
    u64 competing_pointer = 0ULL;
    bool exclusive = !display_direct_prepare(0x0000022000000000ULL,
        &competing_mapping, &competing_info, &competing_pointer) &&
        !competing_pointer && !competing_mapping.size &&
        !display_direct_reclaim(first_pid + 1ULL) && !display_direct_abort() &&
        display_direct_state() == DISPLAY_LEASE_USER &&
        display_direct_owner_process_id() == first_pid && !terminal_render_enabled();
    report("CONCURRENT GOP WRITER / WRONG-OWNER RECLAIM REFUSED", exclusive);
    if (!exclusive) goto fail;

    u64 reply_cookie = 0ULL;
    bool ping = display_service_ping(DISPLAY_SERVICE_TEST_COOKIE, &reply_cookie) &&
        reply_cookie == DISPLAY_SERVICE_TEST_COOKIE;
    report("DISPLAY SERVICE PING", ping);
    if (!ping) goto fail;

    u32 model_checks = 0U;
    bool model = display_service_polaris_model_check(JCOS_DISPLAY_POLARIS_MODEL_VERSION, &model_checks) &&
        model_checks == JCOS_DISPLAY_POLARIS_MODEL_EXPECTED;
    report("POLARIS RING3 READER / DECODER MODEL (NO GPU ACCESS)", model);
    if (!model) goto fail;
    bool model_version_refused = !display_service_polaris_model_check(
        JCOS_DISPLAY_POLARIS_MODEL_VERSION + 1ULL, &model_checks) && !model_checks &&
        display_service_ping(DISPLAY_SERVICE_TEST_COOKIE, &reply_cookie) &&
        reply_cookie == DISPLAY_SERVICE_TEST_COOKIE;
    report("POLARIS MODEL VERSION REFUSED / SERVICE STILL RESPONSIVE", model_version_refused);
    if (!model_version_refused) goto fail;

    bool redraw = display_service_redraw();
    report("USERSPACE FRAMEBUFFER REDRAW", redraw);
    if (!redraw) goto fail;

    bool faulted = display_service_fault();
    ProcessExitInfo exit_info;
    bool exit_record = faulted && display_service_last_exit_info(&exit_info) &&
        exit_info.reason == PROCESS_EXIT_FAULT && exit_info.process_id == first_pid &&
        exit_info.thread_id == first_tid && exit_info.vector == 6ULL;
    bool fault_contained = exit_record && !display_service_running() &&
        display_direct_state() == DISPLAY_LEASE_USER &&
        display_direct_owner_process_id() == first_pid && !terminal_render_enabled();
    report("USER FAULT CONTAINED / LEASE RETAINED", fault_contained);
    if (!fault_contained) goto fail;

    bool recovered = display_service_recover() &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL &&
        terminal_render_enabled() == baseline.terminal_render &&
        baseline_matches(&baseline);
    report("FAULT RECOVERY / KERNEL FRAMEBUFFER RESTORED", recovered);
    if (!recovered) goto fail;

    FramebufferInfo retained;
    bool metadata = framebuffer_info(&retained) &&
        retained.physical_base == info.physical_base && retained.size == info.size &&
        retained.mapping_physical_base == info.mapping_physical_base &&
        retained.mapping_size == info.mapping_size && retained.page_offset == info.page_offset &&
        retained.width == info.width && retained.height == info.height &&
        retained.pixels_per_scanline == info.pixels_per_scanline &&
        retained.pixel_format == info.pixel_format && retained.red_mask == info.red_mask &&
        retained.green_mask == info.green_mask && retained.blue_mask == info.blue_mask &&
        retained.reserved_mask == info.reserved_mask &&
        retained.direct_map_safe == info.direct_map_safe && retained.memory_type == info.memory_type;
    report("GOP METADATA PRESERVED THROUGH SERVICE FAILURE / RECOVERY", metadata);
    if (!metadata) goto fail;

    bool restarted = display_service_start();
    u64 second_pid = display_service_process_id();
    u64 second_incarnation = display_service_incarnation();
    reply_cookie = 0ULL;
    bool replacement = restarted && second_pid && second_pid != first_pid &&
        second_incarnation && second_incarnation != first_incarnation &&
        display_service_ping(DISPLAY_SERVICE_TEST_COOKIE + 1ULL, &reply_cookie) &&
        reply_cookie == DISPLAY_SERVICE_TEST_COOKIE + 1ULL &&
        display_direct_owner_process_id() == second_pid;
    report("RESTART / NEW DISPLAY INCARNATION", replacement);
    if (!replacement) goto fail;

    model_checks = 0U;
    bool replacement_model = display_service_polaris_model_check(
        JCOS_DISPLAY_POLARIS_MODEL_VERSION, &model_checks) &&
        model_checks == JCOS_DISPLAY_POLARIS_MODEL_EXPECTED;
    report("POLARIS MODEL IN FRESH SERVICE AFTER FAULT", replacement_model);
    if (!replacement_model) goto fail;

    bool stopped = display_service_stop() &&
        display_service_state() == MANAGED_SERVICE_STOPPED &&
        display_direct_state() == DISPLAY_LEASE_KERNEL &&
        display_direct_owner_process_id() == 0ULL &&
        baseline_matches(&baseline);
    report("GRACEFUL STOP / RESOURCE BASELINE", stopped);
    if (!stopped) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("R8B RING3 DISPLAY SERVICE TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    {
        bool restored = cleanup_all();
        report("FAILURE CLEANUP / KERNEL DISPLAY RESTORED", restored);
        terminal_set_color(terminal_error_color());
        terminal_writeln("R8B RING3 DISPLAY SERVICE TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}