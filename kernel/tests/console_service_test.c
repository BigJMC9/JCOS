#include "console_service_test.h"

#include "address_space.h"
#include "capability.h"
#include "console_portal.h"
#include "endpoint.h"
#include "ipc.h"
#include "lib.h"
#include "pmm.h"
#include "process.h"
#include "process_exit_queue.h"
#include "program.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "vfs.h"
#include "../../include/console_service_protocol.h"

#define CONSOLE_SERVICE_PATH "/bin/consoleservice.elf"
#define CONSOLE_TEST_TIMEOUT_SECONDS 2ULL
#define CONSOLE_TEST_SUPERVISOR_COOKIE 0x5237434F4E535550ULL

static ProgramInstance g_console_program;
static ConsolePortal g_console_portal;
static Endpoint g_command_endpoint;
static Endpoint g_reply_endpoint;
static CapabilityHandle g_kernel_send;
static CapabilityHandle g_kernel_receive;
static CapabilityHandle g_command_grant;
static CapabilityHandle g_reply_grant;
static bool g_command_created;
static bool g_reply_created;
static bool g_kernel_send_cap;
static bool g_kernel_receive_cap;
static bool g_command_grant_cap;
static bool g_reply_grant_cap;
static u64 g_console_incarnation;
static u64 g_next_console_incarnation = 1ULL;

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 spaces;
    u32 threads;
    u32 endpoints;
    u32 tables;
    u32 exit_queues;
    u32 kernel_caps;
    u64 kernel_threads;
    u64 scheduler_threads;
    u64 supervisor_pid;
    u64 supervisor_tid;
} ConsoleBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static u64 timeout_ticks(void) {
    if (!timer_initialized()) return 0ULL;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * CONSOLE_TEST_TIMEOUT_SECONDS : 0ULL;
}

static bool supervisor_healthy(void) {
    u64 reply = 0ULL;
    return supervisor_running() && supervisor_idle() && supervisor_stack_guarded() &&
        supervisor_ping(CONSOLE_TEST_SUPERVISOR_COOKIE, &reply) &&
        reply == CONSOLE_TEST_SUPERVISOR_COOKIE;
}

static void capture(ConsoleBaseline *baseline) {
    if (!baseline) return;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    baseline->free_pages = pmm_stats().free_pages;
    baseline->processes = process_object_count();
    baseline->spaces = address_space_object_count();
    baseline->threads = thread_object_count();
    baseline->endpoints = endpoint_object_count();
    baseline->tables = capability_table_object_count();
    baseline->exit_queues = process_exit_queue_object_count();
    baseline->kernel_caps = caps ? capability_table_count(caps) : 0U;
    baseline->kernel_threads = kernel ? process_thread_count(kernel) : 0ULL;
    baseline->scheduler_threads = scheduler_thread_count();
    baseline->supervisor_pid = supervisor_process_id();
    baseline->supervisor_tid = supervisor_thread_id();
}

static bool baseline_restored(const ConsoleBaseline *baseline) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    Thread *main = thread_current();
    return kernel && caps && main && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->spaces &&
        thread_object_count() == baseline->threads &&
        endpoint_object_count() == baseline->endpoints &&
        capability_table_object_count() == baseline->tables &&
        process_exit_queue_object_count() == baseline->exit_queues &&
        capability_table_count(caps) == baseline->kernel_caps &&
        process_thread_count(kernel) == baseline->kernel_threads &&
        scheduler_thread_count() == baseline->scheduler_threads &&
        supervisor_process_id() == baseline->supervisor_pid &&
        supervisor_thread_id() == baseline->supervisor_tid &&
        scheduler_preemption_enabled() && supervisor_healthy();
}

static bool console_service_idle(void) {
    return g_console_program.thread_created && g_command_created &&
        g_console_program.thread.state == THREAD_STATE_BLOCKED &&
        !g_console_program.thread.on_run_queue &&
        endpoint_receiver_waiting(&g_command_endpoint);
}

static bool wait_console_idle(void) {
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    while (!console_service_idle()) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        Thread *thread = g_console_program.thread_created ? &g_console_program.thread : 0;
        if (!thread || thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
    return true;
}

static bool wait_console_dead(void) {
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    while (g_console_program.thread.state != THREAD_STATE_DEAD) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (g_console_program.thread.state != THREAD_STATE_READY ||
            !g_console_program.thread.on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
    return !g_console_program.thread.on_run_queue &&
        !g_console_program.thread.interrupt_context_ready && !g_console_program.thread.interrupt_rsp;
}

static bool wait_portal_count(u64 expected) {
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    while (console_portal_write_count(&g_console_portal) != expected) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (!console_portal_active(&g_console_portal)) return false;
        if (g_console_portal.thread.state != THREAD_STATE_READY ||
            !g_console_portal.thread.on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
    return true;
}

static bool cleanup_service(void) {
    if (program_instance_needs_cleanup(&g_console_program) &&
        !program_terminate(&g_console_program)) return false;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;

    /* A failed service can die after a mailbox commit but before the caller
     * consumes it. Drain ownerless transport mailboxes before deleting the
     * kernel's RECEIVE authority so the cleanup path is retryable. */
    if (g_command_created) {
        if (endpoint_receiver_waiting(&g_command_endpoint) ||
            endpoint_sender_waiting(&g_command_endpoint)) return false;
        if (endpoint_message_ready(&g_command_endpoint)) {
            if (!g_command_grant_cap) return false;
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!ipc_try_receive(kernel, g_command_grant, &discard) ||
                endpoint_message_ready(&g_command_endpoint)) return false;
        }
    }
    if (g_reply_created) {
        if (endpoint_receiver_waiting(&g_reply_endpoint) ||
            endpoint_sender_waiting(&g_reply_endpoint)) return false;
        if (endpoint_message_ready(&g_reply_endpoint)) {
            if (!g_kernel_receive_cap) return false;
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!ipc_try_receive(kernel, g_kernel_receive, &discard) ||
                endpoint_message_ready(&g_reply_endpoint)) return false;
        }
    }

    if (g_kernel_send_cap) {
        if (!capability_revoke(caps, g_kernel_send)) return false;
        g_kernel_send_cap = false;
        g_kernel_send = CAPABILITY_INVALID_HANDLE;
    }
    if (g_kernel_receive_cap) {
        if (!capability_revoke(caps, g_kernel_receive)) return false;
        g_kernel_receive_cap = false;
        g_kernel_receive = CAPABILITY_INVALID_HANDLE;
    }
    if (g_command_grant_cap) {
        if (!capability_revoke(caps, g_command_grant)) return false;
        g_command_grant_cap = false;
        g_command_grant = CAPABILITY_INVALID_HANDLE;
    }
    if (g_reply_grant_cap) {
        if (!capability_revoke(caps, g_reply_grant)) return false;
        g_reply_grant_cap = false;
        g_reply_grant = CAPABILITY_INVALID_HANDLE;
    }
    if (g_command_created) {
        if (!endpoint_destroy(&g_command_endpoint)) return false;
        g_command_created = false;
    }
    if (g_reply_created) {
        if (!endpoint_destroy(&g_reply_endpoint)) return false;
        g_reply_created = false;
    }
    g_console_incarnation = 0ULL;
    return console_portal_stop(&g_console_portal);
}

static bool cleanup_all(void) {
    if (!cleanup_service()) return false;
    if (!supervisor_running()) {
        if (!supervisor_recover() || !supervisor_start()) return false;
    }
    return supervisor_healthy();
}

void console_service_test_cleanup_run(void) {
    terminal_writeln("R7 CONSOLE SERVICE CLEANUP RETRY:");
    report("CONSOLE SERVICE / PORTAL RELEASED", cleanup_all());
}

static bool create_transport(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    if (!endpoint_create(&g_command_endpoint)) return false;
    g_command_created = true;
    if (!endpoint_create(&g_reply_endpoint)) return false;
    g_reply_created = true;

    if (!capability_insert(caps, &g_command_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_kernel_send)) return false;
    g_kernel_send_cap = true;
    if (!capability_insert(caps, &g_reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_kernel_receive)) return false;
    g_kernel_receive_cap = true;
    if (!capability_insert(caps, &g_command_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &g_command_grant)) return false;
    g_command_grant_cap = true;
    if (!capability_insert(caps, &g_reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER,
            &g_reply_grant)) return false;
    g_reply_grant_cap = true;
    return true;
}

static bool launch_console_service(void) {
    if (!g_next_console_incarnation) return false;
    g_console_incarnation = g_next_console_incarnation++;
    if (!g_console_incarnation) return false;

    VfsNode *file = vfs_resolve(vfs_root(), CONSOLE_SERVICE_PATH);
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!file || file->type != VFS_FILE || !file->data || !file->size || !caps) return false;

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.startup_grant_count = 3U;

    spec.startup_grants[0].authority_table = caps;
    spec.startup_grants[0].authority_handle = g_command_grant;
    spec.startup_grants[0].object = &g_command_endpoint;
    spec.startup_grants[0].type = CAPABILITY_TYPE_ENDPOINT;
    spec.startup_grants[0].rights = CAPABILITY_RIGHT_RECEIVE;

    spec.startup_grants[1].authority_table = caps;
    spec.startup_grants[1].authority_handle = g_reply_grant;
    spec.startup_grants[1].object = &g_reply_endpoint;
    spec.startup_grants[1].type = CAPABILITY_TYPE_ENDPOINT;
    spec.startup_grants[1].rights = CAPABILITY_RIGHT_SEND;

    if (!console_portal_grant_spec(&g_console_portal,
            &spec.startup_grants[2])) return false;

    spec.startup_argument_count = 2U;
    spec.startup_arguments[0] = g_console_incarnation;
    spec.startup_arguments[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    return program_launch(&g_console_program, &spec) && wait_console_idle();
}

static bool service_call(const IpcMessage *request, IpcMessage *reply) {
    Process *kernel = process_kernel();
    if (!kernel || !request || !reply || !console_service_idle()) return false;
    if (!ipc_try_send(kernel, g_kernel_send, request)) return false;
    k_memset(reply, 0, sizeof(*reply));
    u64 timeout = timeout_ticks();
    return timeout &&
        ipc_receive_blocking_for(kernel, g_kernel_receive, reply, timeout) &&
        wait_console_idle();
}

static bool write_byte(u64 byte, u64 expected_count) {
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 3U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_WRITE_BYTE;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    request.words[2] = byte;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    bool called = service_call(&request, &reply);
    bool valid = called && reply.word_count == 4U &&
        reply.words[0] == JCOS_CONSOLE_SERVICE_REPLY_WRITTEN &&
        reply.words[1] == g_console_incarnation &&
        reply.words[2] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION &&
        reply.words[3] == byte;
    return valid && wait_portal_count(expected_count) &&
        console_portal_last_byte(&g_console_portal) == byte;
}

static bool graceful_stop(void) {
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_SHUTDOWN;
    request.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;

    Process *kernel = process_kernel();
    if (!kernel || !console_service_idle() ||
        !ipc_try_send(kernel, g_kernel_send, &request)) return false;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));
    u64 timeout = timeout_ticks();
    bool replied = timeout &&
        ipc_receive_blocking_for(kernel, g_kernel_receive, &reply, timeout);
    bool reply_ok = replied && reply.word_count == 3U &&
        reply.words[0] == JCOS_CONSOLE_SERVICE_REPLY_STOPPED &&
        reply.words[1] == g_console_incarnation &&
        reply.words[2] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    return reply_ok && wait_console_dead() && program_reap(&g_console_program);
}

void console_service_test_run(void) {
    terminal_writeln("R7 USERSPACE CONSOLE SERVICE / VERSIONED PROTOCOL TEST:");

    bool cleaned = cleanup_all();
    Process *kernel = process_kernel();
    Thread *main = thread_current();
    bool preflight = cleaned && kernel && main && main->process == kernel &&
        main->state == THREAD_STATE_RUNNING && main->on_run_queue &&
        scheduler_thread_count() == 1ULL && scheduler_preemption_enabled() &&
        timer_initialized() && supervisor_healthy();
    report("R6 BASELINE / SUPERVISOR HEALTH", preflight);
    if (!preflight) goto fail;

    ConsoleBaseline baseline;
    capture(&baseline);

    bool portal = console_portal_start(&g_console_portal);
    bool transport = portal && create_transport();
    bool launched = transport && launch_console_service();
    CapabilityTable *child_caps = launched ?
        process_capabilities(&g_console_program.process) : 0;
    void *portal_endpoint = 0;
    bool authority = launched && child_caps &&
        capability_table_count(child_caps) == 3U &&
        capability_lookup_rights(child_caps, g_console_program.startup_handles[2],
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_SEND, &portal_endpoint) &&
        portal_endpoint == &g_console_portal.endpoint &&
        !capability_lookup_rights(child_caps, g_console_program.startup_handles[2],
            CAPABILITY_TYPE_ENDPOINT, CAPABILITY_RIGHT_RECEIVE, &portal_endpoint);
    report("EXPLICIT SEND-ONLY CONSOLE AUTHORITY", authority);
    if (!authority) goto fail;

    u64 before = console_portal_write_count(&g_console_portal);
    IpcMessage mismatch;
    k_memset(&mismatch, 0, sizeof(mismatch));
    mismatch.word_count = 3U;
    mismatch.words[0] = JCOS_CONSOLE_SERVICE_MESSAGE_WRITE_BYTE;
    mismatch.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION + 1ULL;
    mismatch.words[2] = 'X';
    IpcMessage mismatch_reply;
    k_memset(&mismatch_reply, 0, sizeof(mismatch_reply));
    bool versioned = service_call(&mismatch, &mismatch_reply) &&
        mismatch_reply.word_count == 4U &&
        mismatch_reply.words[0] == JCOS_SERVICE_REPLY_INCOMPATIBLE_VERSION &&
        mismatch_reply.words[1] == g_console_incarnation &&
        mismatch_reply.words[2] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION &&
        mismatch_reply.words[3] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION + 1ULL &&
        console_portal_write_count(&g_console_portal) == before;
    report("VERSION MISMATCH -> EXPLICIT RESULT / SERVICE SURVIVES", versioned);
    if (!versioned) goto fail;

    IpcMessage unknown;
    k_memset(&unknown, 0, sizeof(unknown));
    unknown.word_count = 2U;
    unknown.words[0] = 0x7FFFFFFFFFFFFFFFULL;
    unknown.words[1] = JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION;
    IpcMessage unknown_reply;
    k_memset(&unknown_reply, 0, sizeof(unknown_reply));
    bool unknown_ok = service_call(&unknown, &unknown_reply) &&
        unknown_reply.word_count == 4U &&
        unknown_reply.words[0] == JCOS_SERVICE_REPLY_UNKNOWN_OPERATION &&
        unknown_reply.words[1] == g_console_incarnation &&
        unknown_reply.words[2] == JCOS_CONSOLE_SERVICE_PROTOCOL_VERSION &&
        unknown_reply.words[3] == unknown.words[0] &&
        console_portal_write_count(&g_console_portal) == before;
    report("UNKNOWN OPERATION -> EXPLICIT RESULT / SERVICE SURVIVES", unknown_ok);
    if (!unknown_ok) goto fail;

    bool output = write_byte('R', before + 1ULL) &&
        write_byte('7', before + 2ULL) &&
        write_byte('\n', before + 3ULL);
    report("USERSPACE SERVICE -> PRIVILEGED BYTE PORTAL", output);
    if (!output) goto fail;

    bool stopped = graceful_stop();
    bool released = stopped && cleanup_service();
    bool final = released && baseline_restored(&baseline);
    report("GRACEFUL STOP / RESOURCE BASELINE", final);

    if (final) {
        terminal_set_color(terminal_accent_color());
        terminal_writeln("R7 USERSPACE CONSOLE SERVICE / VERSIONED PROTOCOL TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }

fail:
    {
        bool restored = cleanup_all();
        report("FAILURE CLEANUP / SUPERVISOR RESTORED", restored);
        terminal_set_color(terminal_error_color());
        terminal_writeln("R7 USERSPACE CONSOLE SERVICE / VERSIONED PROTOCOL TEST: FAILED");
        terminal_set_color(terminal_default_color());
    }
}