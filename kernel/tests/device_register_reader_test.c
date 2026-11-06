#include "device_register_reader_test.h"
#include "device_register_reader.h"
#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "lib.h"
#include "managed_service.h"
#include "pmm.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "system_console.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "register_probe_protocol.h"

static ManagedService g_probe;
static DeviceRegisterReader g_reader;
static Process g_other;
static CapabilityHandle g_cap;
static CapabilityHandle g_other_cap;
static CapabilityHandle g_write_cap;
static ManagedServiceSpec probe_spec(void) {
    ManagedServiceSpec spec;
    spec.path = "/bin/registerprobe.elf";
    spec.shutdown_message = REGISTER_PROBE_SHUTDOWN;
    spec.shutdown_reply = REGISTER_PROBE_STOPPED;
    return spec;
}
typedef struct {
    u64 pages;
    u32 processes, spaces, threads, tables, endpoints, queues, devices, readers;
    u32 kernel_caps;
    u64 runnable;
} Baseline;
static void capture(Baseline *b) {
    b->pages = pmm_stats().free_pages;
    b->processes = process_object_count();
    b->spaces = address_space_object_count();
    b->threads = thread_object_count();
    b->tables = capability_table_object_count();
    b->endpoints = endpoint_object_count();
    b->queues = process_exit_queue_object_count();
    b->devices = device_resource_object_count();
    b->readers = device_register_reader_object_count();
    b->kernel_caps = capability_table_count(process_capabilities(process_kernel()));
    b->runnable = scheduler_thread_count();
}
static bool matches(const Baseline *b) {
    Baseline after;
    k_memset(&after, 0, sizeof(after));
    capture(&after);
    return b->pages == after.pages && b->processes == after.processes &&
        b->spaces == after.spaces && b->threads == after.threads &&
        b->tables == after.tables && b->endpoints == after.endpoints &&
        b->queues == after.queues && b->devices == after.devices &&
        b->readers == after.readers && b->kernel_caps == after.kernel_caps &&
        b->runnable == after.runnable;
}
static void report(const char *step, bool pass) {
    terminal_write("  "); terminal_write(step); terminal_writeln(pass ? ": PASS" : ": FAILED");
}
static bool cleanup(void) {
    if (device_register_reader_valid(&g_reader)) (void)device_register_reader_revoke(&g_reader);
    ManagedServiceState state = managed_service_state(&g_probe);
    if (state == MANAGED_SERVICE_RUNNING) {
        ManagedServiceStopResult stop = managed_service_stop_bounded(&g_probe);
        if (stop != MANAGED_SERVICE_STOP_GRACEFUL && stop != MANAGED_SERVICE_STOP_FORCED) return false;
    } else if (state != MANAGED_SERVICE_STOPPED && !managed_service_recover(&g_probe)) return false;
    /* Process teardown removes all of its test-reader capabilities. */
    g_cap = g_write_cap = CAPABILITY_INVALID_HANDLE;
    if (process_storage_in_use(&g_other)) {
        CapabilityTable *caps = process_capabilities(&g_other);
        if (g_other_cap) {
            if (!capability_revoke(caps, g_other_cap)) return false;
            g_other_cap = CAPABILITY_INVALID_HANDLE;
        }
        if (!process_destroy(&g_other)) return false;
    }
    return !device_register_reader_valid(&g_reader) || device_register_reader_destroy(&g_reader);
}
void device_register_reader_test_cleanup_run(void) {
    terminal_writeln("DEVICE REGISTER READER CLEANUP:");
    report("READER / PROBE RESOURCES RELEASED", cleanup());
}
static void fixture(u32 *values) {
    k_memset(values, 0, DEVICE_REGISTER_TEST_VALUES * sizeof(u32));
    for (u32 pipe = 0; pipe < POLARIS_DISPLAY_PIPE_COUNT; ++pipe) {
        u32 *v = values + pipe * POLARIS_DISPLAY_REGISTER_COUNT;
        v[POLARIS_REG_CRTC_CONTROL] = 0x10001U;
        v[POLARIS_REG_GRPH_ENABLE] = 1U;
        v[POLARIS_REG_GRPH_CONTROL] = 2U;
        v[POLARIS_REG_PRIMARY_LOW] = 0x200000U;
        v[POLARIS_REG_PRIMARY_HIGH] = 1U;
        v[POLARIS_REG_PITCH] = v[POLARIS_REG_X_END] = 1024U;
        v[POLARIS_REG_Y_END] = 768U;
    }
}
static bool request(const ManagedServiceConnection *connection, u64 operation,
    u64 cap, u64 identity, u64 argument, IpcMessage *reply) {
    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    k_memset(reply, 0, sizeof(*reply));
    message.word_count = 4U;
    message.words[0] = operation;
    message.words[1] = cap;
    message.words[2] = identity;
    message.words[3] = argument;
    return managed_service_call(&g_probe, connection, &message, reply) &&
        reply->word_count == 4U && reply->words[0] == REGISTER_PROBE_REPLY &&
        reply->words[1] == connection->incarnation;
}
static bool remove_reader(Process *owner) {
    if (!device_register_reader_valid(&g_reader)) return !g_cap;
    if (!device_register_reader_revoke(&g_reader)) return false;
    if (g_cap && !capability_revoke(process_capabilities(owner), g_cap)) return false;
    g_cap = 0;
    return device_register_reader_destroy(&g_reader);
}
void device_register_reader_test_run(void) {
    terminal_writeln("PROTECTED DEVICE REGISTER READER TEST:");
    bool ready = cleanup() && scheduler_preemption_enabled() && timer_initialized();
    report("CLEAN BASELINE / LIVE PREEMPTION", ready);
    if (!ready) goto fail;
    Baseline base;
    k_memset(&base, 0, sizeof(base));
    capture(&base);
    ManagedServiceSpec spec = probe_spec();
    bool started = managed_service_start(&g_probe, &spec);
    ManagedServiceConnection connection;
    bool connected = started && managed_service_connect_kernel(&g_probe, &connection);
    report("RING3 PROBE / IPC CONNECTION", connected);
    if (!connected) goto fail;
    Process *owner = &g_probe.program.process;
    u32 values[DEVICE_REGISTER_TEST_VALUES];
    fixture(values);
    CapabilityHandle rejected = ~0ULL;
    bool closed = !device_register_reader_create_test_bank(&g_reader, owner,
        JCOS_REGISTER_PROFILE_POLARIS_DCE112, values, DEVICE_REGISTER_TEST_VALUES, &rejected) &&
        !rejected && !device_register_reader_valid(&g_reader);
    report("PHYSICAL PROFILE REFUSED / NO GPU ACCESS", closed);
    if (!closed) goto fail;
    bool created = device_register_reader_create_test_bank(&g_reader, owner,
        JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST, values, DEVICE_REGISTER_TEST_VALUES, &g_cap);
    report("KERNEL-OWNED TEST BANK / READ-ONLY CAPABILITY", created);
    if (!created) goto fail;
    u64 identity = g_reader.resource.id;
    CapabilityTable *caps = process_capabilities(owner);
    void *resolved = 0;
    bool attenuated = capability_lookup_rights(caps, g_cap, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_READ, &resolved) && resolved == &g_reader.resource &&
        !capability_lookup_rights(caps, g_cap, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_WRITE, &resolved) &&
        !capability_lookup_rights(caps, g_cap, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_MANAGE, &resolved) &&
        !capability_lookup_rights(caps, g_cap, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_TRANSFER, &resolved);
    rejected = ~0ULL;
    bool duplicate = !device_register_reader_create_test_bank(&g_reader, owner,
        JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST, values, DEVICE_REGISTER_TEST_VALUES, &rejected) &&
        !rejected && g_reader.resource.id == identity;
    report("READ ONLY / NONTRANSFERABLE / LIVE REINITIALIZATION REFUSED", attenuated && duplicate);
    if (!attenuated || !duplicate) goto fail;
    IpcMessage reply;
    bool captured = request(&connection, REGISTER_PROBE_CHECK, g_cap, identity,
        JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST, &reply) &&
        reply.words[2] == REGISTER_PROBE_EXPECTED && g_reader.successful_reads == 30ULL;
    report("RING3 POLARIS CAPTURE / MALFORMED REQUEST REFUSALS", captured);
    if (!captured) goto fail;
    u32 value = ~0U;
    values[3U * POLARIS_DISPLAY_REGISTER_COUNT + POLARIS_REG_PITCH] = 7U;
    bool immutable = device_register_reader_read(owner, g_cap, identity,
        JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &value) && value == 1024U;
    report("COPIED TEST BANK / DRIVER CANNOT CHANGE VALUES", immutable);
    if (!immutable) goto fail;
    bool rights = capability_insert(caps, &g_reader.resource, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_WRITE, &g_write_cap) &&
        !device_register_reader_read(owner, g_write_cap, identity,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &value) && !value &&
        capability_revoke(caps, g_write_cap);
    if (rights) g_write_cap = 0;
    report("MISSING READ RIGHTS REFUSED", rights);
    if (!rights) goto fail;
    bool other = process_create(&g_other) &&
        capability_insert(process_capabilities(&g_other), &g_reader.resource,
            CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_READ, &g_other_cap) &&
        !device_register_reader_read(&g_other, g_other_cap, identity,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &value) && !value;
    report("FOREIGN OWNER WITH CAPABILITY REFUSED", other);
    if (!other) goto fail;
    bool pinned = !device_register_reader_destroy(&g_reader) &&
        device_register_reader_valid(&g_reader) && device_register_reader_revoke(&g_reader) &&
        request(&connection, REGISTER_PROBE_READ, g_cap, identity,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &reply) &&
        !reply.words[2] && !reply.words[3];
    report("REVOKED READER DENIED IN RING3 / CAP REFS RETAIN OBJECT", pinned);
    if (!pinned) goto fail;
    if (!capability_revoke(process_capabilities(&g_other), g_other_cap)) goto fail;
    g_other_cap = 0;
    if (!process_destroy(&g_other) || !capability_revoke(caps, g_cap)) goto fail;
    CapabilityHandle old_cap = g_cap;
    g_cap = 0;
    if (!device_register_reader_destroy(&g_reader) ||
        !device_register_reader_create_test_bank(&g_reader, owner,
            JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST, values, DEVICE_REGISTER_TEST_VALUES, &g_cap)) goto fail;
    bool fresh = g_reader.resource.id != identity &&
        request(&connection, REGISTER_PROBE_READ, old_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &reply) &&
        !reply.words[2] && !reply.words[3] &&
        request(&connection, REGISTER_PROBE_READ, g_cap, identity,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &reply) &&
        !reply.words[2] && !reply.words[3] &&
        request(&connection, REGISTER_PROBE_READ, g_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &reply) &&
        reply.words[2] == 1ULL && reply.words[3] == 7ULL;
    report("RECREATED RESOURCE / OLD HANDLE AND ID REFUSED", fresh);
    if (!fresh) goto fail;
    /* Finite capture goes through the same syscall and userspace decoder,
     * using a distinct one-shot copied-RAM profile. */
    if (!remove_reader(owner)) goto fail;
    fixture(values);
    u64 duration = (u64)timer_frequency() * POLARIS_CAPTURE_MAX_SECONDS;
    rejected = ~0ULL;
    bool limits = !device_register_reader_create_test_capture(&g_reader, owner, 3U, values,
        DEVICE_REGISTER_TEST_VALUES, 0ULL, &rejected) && !rejected &&
        !device_register_reader_create_test_capture(&g_reader, owner, 6U, values,
        DEVICE_REGISTER_TEST_VALUES, duration, &rejected) && !rejected &&
        !device_register_reader_create_test_capture(&g_reader, owner, 3U, values,
        DEVICE_REGISTER_TEST_VALUES, duration + 1ULL, &rejected) && !rejected &&
        !device_register_reader_create_test_bank(&g_reader, owner,
        JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST, values, DEVICE_REGISTER_TEST_VALUES, &rejected) &&
        !rejected && !device_register_reader_valid(&g_reader);
    report("FINITE CAPTURE FACTORY / INVALID PIPE AND BUDGET REFUSALS", limits);
    if (!limits) goto fail;
    bool finite = device_register_reader_create_test_capture(&g_reader, owner, 3U, values,
        DEVICE_REGISTER_TEST_VALUES, duration, &g_cap) &&
        !device_register_reader_create_test_capture(&g_reader, owner, 3U, values,
            DEVICE_REGISTER_TEST_VALUES, duration, &rejected) && !rejected &&
        request(&connection, REGISTER_PROBE_CHECK, g_cap, g_reader.resource.id,
            JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST, &reply) &&
        reply.words[2] == REGISTER_PROBE_EXPECTED && g_reader.successful_reads == 30ULL &&
        g_reader.capture.admitted == POLARIS_CAPTURE_READ_LIMIT && g_reader.capture.closed &&
        g_reader.revoked &&
        request(&connection, REGISTER_PROBE_READ, g_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_CRTC_CONTROL), &reply) &&
        !reply.words[2] && !reply.words[3] && !device_register_reader_destroy(&g_reader);
    report("ONE-SHOT RING3 CAPTURE / THIRTY READS / SEALED WITH REFS RETAINED", finite);
    if (!finite || !remove_reader(owner)) goto fail;
    bool order = device_register_reader_create_test_capture(&g_reader, owner, 3U, values,
        DEVICE_REGISTER_TEST_VALUES, duration, &g_cap) &&
        request(&connection, REGISTER_PROBE_READ, g_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &reply) &&
        !reply.words[2] && !reply.words[3] && g_reader.revoked &&
        !g_reader.capture.admitted && !g_reader.successful_reads &&
        request(&connection, REGISTER_PROBE_READ, g_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_CRTC_CONTROL), &reply) &&
        !reply.words[2] && !reply.words[3];
    report("WRONG ORDER CLOSES CAPTURE / NO READ / NO REARM", order);
    if (!order || !remove_reader(owner)) goto fail;
    values[3U * POLARIS_DISPLAY_REGISTER_COUNT + POLARIS_REG_CRTC_CONTROL] = ~0U;
    bool absent = device_register_reader_create_test_capture(&g_reader, owner, 3U, values,
        DEVICE_REGISTER_TEST_VALUES, duration, &g_cap) &&
        request(&connection, REGISTER_PROBE_READ, g_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_CRTC_CONTROL), &reply) &&
        !reply.words[2] && !reply.words[3] && g_reader.revoked &&
        g_reader.capture.admitted == 1U && !g_reader.successful_reads;
    report("ALL-ONES VALUE CLOSES CAPTURE / CLEARED RESULT", absent);
    if (!absent || !remove_reader(owner)) goto fail;
    fixture(values);
    bool hung = device_register_reader_create_test_capture(&g_reader, owner, 3U, values,
        DEVICE_REGISTER_TEST_VALUES, timer_frequency(), &g_cap);
    IpcMessage spin;
    k_memset(&spin, 0, sizeof(spin));
    spin.word_count = 4U;
    spin.words[0] = REGISTER_PROBE_SPIN;
    spin.words[1] = g_cap; spin.words[2] = g_reader.resource.id; spin.words[3] = 3U;
    hung = hung && managed_service_send_oneway(&g_probe, &connection, &spin);
    u64 wait_start = timer_ticks();
    while (hung && !g_reader.successful_reads &&
        timer_ticks() - wait_start < (u64)timer_frequency() * 2ULL) (void)scheduler_yield();
    hung = hung && g_reader.successful_reads == 1U;
    u64 console_writes = system_console_portal_write_count();
    u64 console_process = system_console_process_id();
    bool progress = hung && system_console_running() &&
        system_console_writeln("REGISTER CAPTURE: INDEPENDENT CONSOLE PROGRESS") &&
        system_console_process_id() == console_process &&
        system_console_portal_write_count() > console_writes &&
        managed_service_state(&g_probe) == MANAGED_SERVICE_RUNNING;
    report("SPINNING READER / INDEPENDENT RING3 CONSOLE PROGRESS", progress);
    if (!progress) goto fail;
    while (timer_ticks() - g_reader.capture.started_at < g_reader.capture.duration)
        (void)scheduler_yield();
    bool expired = !device_register_reader_read(owner, g_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_CRTC_BLANK), &value) && !value &&
        g_reader.revoked && g_reader.capture.closed && g_reader.successful_reads == 1U &&
        g_reader.capture.admitted == 1U && !device_register_reader_destroy(&g_reader);
    report("PARTIAL CAPTURE DEADLINE / NO FURTHER READ / RESOURCE RETAINED", expired);
    if (!expired) goto fail;
    bool forced = managed_service_stop_bounded(&g_probe) == MANAGED_SERVICE_STOP_FORCED &&
        device_register_reader_valid(&g_reader) && cleanup() && matches(&base);
    report("SPINNING OWNER FORCED STOP / REAP / FULL BASELINE", forced);
    if (!forced || !managed_service_start(&g_probe, &spec) ||
        !managed_service_connect_kernel(&g_probe, &connection)) goto fail;
    owner = &g_probe.program.process;
    bool partial = device_register_reader_create_test_capture(&g_reader, owner, 3U, values,
        DEVICE_REGISTER_TEST_VALUES, duration, &g_cap) &&
        request(&connection, REGISTER_PROBE_READ, g_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_CRTC_CONTROL), &reply) &&
        reply.words[2] == 1ULL && g_reader.capture.admitted == 1U;
    report("FRESH PROCESS / PARTIAL FINITE CAPTURE BEFORE FAULT", partial);
    if (!partial) goto fail;
    IpcMessage fault;
    k_memset(&fault, 0, sizeof(fault));
    fault.word_count = 1U;
    fault.words[0] = REGISTER_PROBE_FAULT;
    bool sent = managed_service_send_oneway(&g_probe, &connection, &fault);
    u64 started_at = timer_ticks();
    u64 timeout = (u64)timer_frequency() * 2ULL;
    while (sent && managed_service_state(&g_probe) == MANAGED_SERVICE_RUNNING &&
        timer_ticks() - started_at < timeout) (void)scheduler_yield();
    ProcessExitInfo exit;
    bool contained = sent && managed_service_last_exit_info(&g_probe, &exit) &&
        exit.reason == PROCESS_EXIT_FAULT && exit.vector == 6ULL &&
        !device_register_reader_read(owner, g_cap, g_reader.resource.id,
            JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH), &value) && !value &&
        device_register_reader_valid(&g_reader) && g_reader.capture.admitted == 1U &&
        g_reader.successful_reads == 1U;
    report("OWNER FAULT / READS DENIED / RESOURCE RETAINED", contained);
    if (!contained) goto fail;
    bool restored = cleanup() && matches(&base);
    report("PROCESS REAP / RESOURCE BASELINE RESTORED", restored);
    terminal_writeln(restored ? "PROTECTED DEVICE REGISTER READER TEST: PASS" :
        "PROTECTED DEVICE REGISTER READER TEST: FAILED");
    return;
fail:
    report("FAILURE CLEANUP", cleanup());
    terminal_writeln("PROTECTED DEVICE REGISTER READER TEST: FAILED");
}
